/*
  CONTROL DE MOTOR PASO A PASO CON ESP32 POR WIFI
  ===============================================

  El ESP32 sirve una pagina web sencilla para mover un motor paso a paso con
  un driver de entradas STEP y DIR (A4988, DRV8825, TMC2208, TB6600, DM542...)
  y encender dos reles temporizados: cada uno por su lado o los dos en serie
  (primero uno durante su tiempo y, al apagarse, el otro durante el suyo).
  El giro se elige en la web con tres deslizadores: velocidad (rpm), angulo
  de avance (a la derecha) y angulo de retroceso (a la izquierda), de 0 a 360
  grados cada uno.
  El DISPARO junta las dos cosas: avanza su angulo, retrocede el suyo, espera
  DISPARO_ESPERA_MS y enciende el rele 1, el 2, o los dos en serie.
  SENSOR DE PIENSO (maqueta, sin sensor aun): la web deja elegida la
  secuencia de reles que se lanzara sola al detectar pienso; se guarda en la
  memoria del ESP32 y se prueba con el boton "Simular deteccion".

  MODOS WIFI
    - Al arrancar crea su propia red ESP32_MOTOR (modo AP), web en
      http://192.168.4.1. LED parpadeando.
    - Una pulsacion corta del boton BOOT (pasados ARRANQUE_GRACIA_MS desde el
      arranque) para el motor y los reles y pasa a modo host: busca la WiFi de
      casa y despues la segunda red, TIEMPO_CONEXION_MS cada una. LED
      parpadeando el doble de rapido.
    - Si conecta: LED fijo, web en http://motor.local o en la IP que sale por
      el Monitor Serie. Si no conecta a ninguna, vuelve al modo AP.
    - Otra pulsacion de BOOT para el motor y los reles y vuelve al modo AP.

  USO
    1. Poner nombre y clave de las WiFi en credenciales.h (hay un ejemplo en
       credenciales.h.ejemplo). Ese fichero no se sube a GitHub.
    2. Cargar este programa en el ESP32 (placa "ESP32 Dev Module").
    3. Conectarse a ESP32_MOTOR (clave 12345678) y abrir http://192.168.4.1,
       o pulsar BOOT para usar la WiFi de casa.

  CONEXIONES (con los valores por defecto de abajo)

    ESP32 (WROOM)       Driver
    -----               ------
    GPIO 33  (STEP_PIN) STEP   (en drivers optoacoplados tipo TB6600: PUL+)
    GPIO 32  (DIR_PIN)  DIR    (en drivers optoacoplados tipo TB6600: DIR+)
    GPIO 25  (ENABLE_PIN) EN   corta la corriente del motor en reposo
                               (en drivers optoacoplados tipo TB6600: ENA+)

    ESP32                Modulo de 2 reles
    -----                -----------------
    GPIO 27  (RELE1_PIN) IN1   rele 1
    GPIO 13  (RELE2_PIN) IN2   rele 2
    GND                  GND
    VIN (5 V del USB)   VCC   (si el modulo es de 5 V)
    GND                 GND logico del driver (en TB6600: PUL- y DIR-)

    En la placa de desarrollo, D33 y D32 estan justo al lado de D35 y D34.
    NO usar D34, D35, D36 (VP) ni D39 (VN): en el ESP32 son SOLO DE ENTRADA y
    no pueden dar senal al driver. El programa no compila si se ponen.

    - El ESP32 trabaja a 3,3 V. A4988, DRV8825 y TMC2208 aceptan 3,3 V
      directamente. Algunos drivers optoacoplados necesitan 5 V en la entrada:
      si el motor no responde, revisa la hoja del driver.
    - Si el driver tiene entrada ENABLE, dejala habilitada (en el A4988 y el
      DRV8825 viene habilitada por defecto si no se conecta).
    - La alimentacion del motor va al driver, nunca al ESP32. Une las masas.

  COMO SE GENERAN LOS PULSOS
    Con un bucle de delayMicroseconds() el ESP32 se quedaria ocupado hasta que
    el motor terminara: 10 vueltas a 2 ms por paso son 4 segundos sin atender
    la web ni el boton Parar.
    Generarlos en loop() comparando con micros() tampoco basta: mientras la web
    responde una peticion (unos milisegundos) no salen pulsos, el motor se
    frena de golpe y se oye un "pum".
    Por eso los pulsos los da un temporizador hardware del ESP32 que salta a
    intervalos fijos (segun la velocidad), pase lo que pase con la web. Su
    interrupcion corre en el nucleo 1; la WiFi va en el nucleo 0. Con el
    temporizador de software (esp_timer), que corre en el nucleo de la WiFi,
    los pulsos se retrasaban hasta 0,5 ms en modo AP y casi 1 ms buscando red.
*/

#include <WiFi.h>
#include <ESPmDNS.h>          // nombre motor.local (viene con el core ESP32)
#include "esp_timer.h"        // esp_timer_get_time(), para medir los pulsos (viene con el core)
#include "hal/gpio_ll.h"      // escritura directa de STEP en la interrupcion (viene con el core)
#include "esp_http_server.h"  // servidor web del sistema del ESP32 (viene con el core)
#include "lwip/sockets.h"     // para saber la IP de quien hace cada peticion
#include <Preferences.h>      // memoria que no se borra al apagar (viene con el core)

// Redes WiFi: WIFI_ROUTER_SSID / WIFI_ROUTER_PASSWORD (la primera que se
// prueba) y WIFI_ROUTER2_SSID / WIFI_ROUTER2_PASSWORD (la segunda, opcional)
#if __has_include("credenciales.h")
#include "credenciales.h"
#else
#error "Falta credenciales.h: copia credenciales.h.ejemplo como credenciales.h y pon tu WiFi."
#endif
#ifndef WIFI_ROUTER2_SSID          // segunda red sin configurar: se salta
#define WIFI_ROUTER2_SSID     ""
#define WIFI_ROUTER2_PASSWORD ""
#endif

// =====================================================================
//  PARAMETROS CONFIGURABLES
// =====================================================================

#define STEP_PIN            33      // GPIO conectado a STEP del driver (D33)
#define DIR_PIN             32      // GPIO conectado a DIR del driver  (D32)
#define ENABLE_PIN          25      // GPIO conectado a EN del driver   (D25)
#define ENABLE_ACTIVO       LOW     // nivel de EN que activa el driver (DRV8825 y A4988: LOW)

// Corriente en reposo. Parado, el driver mantiene las bobinas con corriente
// para que el eje no se mueva (par de retencion), y motor y driver se
// calientan. Con true, el programa corta la corriente por EN cuando el motor
// lleva REPOSO_MS parado y la vuelve a dar justo antes de moverse.
// Pon false si el eje lleva una carga que lo pueda mover estando parado.
#define DESACTIVAR_EN_REPOSO true
#define REPOSO_MS           500     // tiempo parado antes de cortar la corriente
#define ENABLE_ESPERA_MS    5       // espera tras activar el driver antes del primer paso

// Dos reles temporizados: cada boton de la web enciende su rele durante el
// tiempo indicado y luego se apaga solo. La activacion en serie enciende uno
// y, cuando se apaga, el otro, cada uno con su tiempo.
#define RELE1_PIN           27      // GPIO conectado a la entrada IN1 del modulo de reles (D27)
#define RELE2_PIN           13      // GPIO conectado a la entrada IN2 del modulo de reles (D13)
                                    // No usar GPIO 14 ni 15: dan pulsos durante el arranque
                                    // del ESP32 y el rele chasquearia al encender
#define RELE_ACTIVO         HIGH    // nivel que enciende los reles. Muchos modulos con
                                    // optoacoplador se activan con LOW: si el rele se
                                    // enciende al arrancar y se apaga al pulsar, pon LOW
#define RELE_TIEMPO_S       1       // tiempo por defecto en la web, en segundos (los dos reles)
#define RELE_MAX_S          600     // tiempo maximo por orden, en segundos

// Disparo: el motor avanza, retrocede, se para, espera este tiempo y
// enciende los reles segun el modo elegido en la web.
#define DISPARO_ESPERA_MS   1000

#define FOTO1_PIN            15      // Fotodetector 1: D15, activo a nivel LOW
#define FOTO2_PIN            16      // Fotodetector 2: D16, activo a nivel LOW

// Sensor de pienso: PENDIENTE. De momento la web solo guarda la secuencia
// automatica y el boton "Simular deteccion" la lanza. Cuando haya sensor,
// leerlo en loop() y llamar a piensoDetectado() al detectar.

// Pulsos que necesita el driver para una vuelta completa del motor.
// Motor de 200 pasos sin microstepping = 200.
// Con microstepping se multiplica: 1/2 = 400, 1/8 = 1600, 1/16 = 3200...
#define PULSOS_POR_VUELTA   200

// Velocidad al encender, en vueltas por minuto. Despues se cambia con el
// deslizador de velocidad de la web, entre RPM_MIN y RPM_MAX. Sin rampa de
// aceleracion, a mucha velocidad el motor puede perder pasos: si pasa, baja
// RPM_MAX.
#define RPM_INICIAL         25
#define RPM_MIN             10
#define RPM_MAX             300

// LED de estado de la WiFi (el LED azul de la placa suele estar en GPIO 2):
//   parpadeo                  = modo AP, red propia ESP32_MOTOR
//   parpadeo el doble de rapido = modo host buscando red (o conexion perdida)
//   fijo                      = modo host conectado
#define LED_PIN              2      // LED de estado WiFi en GPIO2
#define LED_ENCENDIDO       HIGH    // cambia a LOW si tu LED se enciende al reves
#define LED_AP_MS           500     // medio periodo del parpadeo en modo AP
                                    // (buscando red: la mitad, el doble de rapido)

// Boton BOOT de la placa (GPIO 0): una pulsacion corta cambia entre modo AP y
// modo host. Se ignora durante ARRANQUE_GRACIA_MS tras el arranque y hasta que
// se vea suelto, para que no cuente si seguia pulsado al encender o al cargar.
#define BOTON_MODO_PIN      0
#define ARRANQUE_GRACIA_MS  3000
#define ANTIRREBOTE_MS      50

// Tiempo maximo intentando conectar a cada red en modo host.
#define TIEMPO_CONEXION_MS  15000

// Nombre en la red: la web se abre tambien en http://motor.local
#define NOMBRE_RED          "motor"

// Red propia (modo AP, el de arranque)
#define WIFI_SSID           "ESP32_MOTOR"
#define WIFI_PASSWORD       "12345678"   // minimo 8 caracteres
#define WIFI_CANAL          6            // canal WiFi de 2,4 GHz (1 a 13)

// Potencia de emision de la red propia. En muchas placas clonicas la red no
// aparece con la potencia maxima porque los picos de consumo hacen caer la
// tension; con 8,5 dBm se ve bien a varios metros. Otras opciones:
// WIFI_POWER_11dBm, WIFI_POWER_15dBm, WIFI_POWER_19_5dBm (maxima).
#define WIFI_POTENCIA       WIFI_POWER_8_5dBm

// Nivel de DIR que corresponde a "derecha". Si el motor gira al reves de lo
// que indica la web, cambia HIGH por LOW.
#define DIR_DERECHA         HIGH

// Espera tras cambiar DIR antes del primer pulso (los drivers piden entre
// 0,2 y 5 microsegundos).
#define DIR_SETUP_US        5

// Limite de vueltas por orden, para evitar errores al teclear.
#define VUELTAS_MAX         1000

// Servidor web: el del sistema del ESP32 (esp_http_server). Atiende varias
// conexiones a la vez en su propia tarea, y cuando se llena cierra la menos
// usada para dejar entrar a la nueva (lo que pasa al recargar la pagina).
#define WEB_MAX_CONEXIONES  7       // conexiones abiertas a la vez como maximo
#define WEB_ESPERA_S        2       // segundos maximos esperando a un navegador que
                                    // no envia ni recibe (el sistema trae 5)

// Depuracion por el Monitor Serie (115200 baudios): dispositivos que se
// conectan a la red propia, conexion con el router y cada peticion a la web,
// con la hora en segundos desde el arranque. DEPURAR_ESTADO muestra tambien
// las consultas /estado que la pagina hace cada segundo (por defecto no, para
// que no tapen lo demas).
#define DEPURACION          true
#define DEPURAR_ESTADO      false

// Cada DIAGNOSTICO_MS, con DEPURACION, una linea [sistema] con las peticiones
// atendidas, el envio mas lento, la memoria libre y los dispositivos
// conectados a la red propia. Si la web se frena, esta linea dice por que.
#define DIAGNOSTICO_MS      30000

// =====================================================================

// Los GPIO 34 a 39 del ESP32 son solo de entrada: no pueden mover el driver.
#if (STEP_PIN >= 34 && STEP_PIN <= 39) || (DIR_PIN >= 34 && DIR_PIN <= 39) || (ENABLE_PIN >= 34 && ENABLE_PIN <= 39) || (RELE1_PIN >= 34 && RELE1_PIN <= 39) || (RELE2_PIN >= 34 && RELE2_PIN <= 39)
#error "STEP_PIN, DIR_PIN, ENABLE_PIN, RELE1_PIN y RELE2_PIN no pueden ser los GPIO 34 a 39: en el ESP32 son solo de entrada. Usa por ejemplo 33, 32, 25, 27 y 13."
#endif
#if RELE1_PIN == RELE2_PIN
#error "RELE1_PIN y RELE2_PIN tienen que ser GPIO distintos."
#endif

// El temporizador del ESP32 no admite periodos menores de 50 us.
#if RPM_INICIAL < RPM_MIN || RPM_INICIAL > RPM_MAX
#error "RPM_INICIAL tiene que estar entre RPM_MIN y RPM_MAX."
#endif

httpd_handle_t servidor = NULL;

// Respuesta de una orden. Las ordenes (/mover, /estado...) son las mismas por
// la web y por el puerto serie: cada una recibe la consulta y devuelve esto.
// (Va aqui arriba porque el IDE declara las funciones al principio.)
const char* TEXTO = "text/plain; charset=utf-8";
const char* JSON = "application/json";
struct Respuesta {
  int codigo;
  const char* tipo;
  String cuerpo;
};

// La web se atiende en la tarea del servidor y el resto (fin de movimiento,
// reles, boton, WiFi) en loop(). Este candado evita que las dos toquen el
// estado del motor y de los reles a la vez.
SemaphoreHandle_t candadoEstado;

// Estado del movimiento. moverMotor() lo prepara y el temporizador da los
// pulsos. 'volatile' porque el temporizador corre en otra tarea del ESP32.
volatile long pulsosPendientes = 0;   // pulsos que faltan por dar
volatile bool stepEnAlto = false;     // estado actual de la salida STEP
bool sentidoDerecha = true;           // sentido del movimiento en curso
bool temporizadorEnMarcha = false;
bool driverActivo = false;            // true = bobinas con corriente
unsigned long finMovimiento = 0;      // instante (millis) en que paro el motor
unsigned long pulsoUs = 0;            // periodo del temporizador: fija la velocidad
                                      // (lo calcula cambiarVelocidad en setup)
int rpmActual = 0;                    // velocidad en vueltas por minuto (se calcula en setup)

// Estado de cada rele (reles[0] es el rele 1 y reles[1] el rele 2)
struct Rele {
  int pin;
  bool encendido;
  unsigned long inicio;               // instante (millis) en que se encendio
  unsigned long duracionMs;           // cuanto tiempo tiene que estar encendido
};
Rele reles[] = {
  { RELE1_PIN, false, 0, 0 },
  { RELE2_PIN, false, 0, 0 },
};
const int NUM_RELES = sizeof(reles) / sizeof(reles[0]);

// Activacion en serie: cuando se apague el rele 'serieTras', se enciende el
// rele 'serieSiguiente' durante 'serieMs'. -1 = no hay ninguna pendiente.
int serieTras = -1;
int serieSiguiente = -1;
unsigned long serieMs = 0;

// Disparo: avance, retroceso (si lo hay), espera de DISPARO_ESPERA_MS y
// reles. disparoModo: "1", "2", "12" (rele 1 y luego rele 2) o "21" (rele 2
// y luego rele 1).
enum FaseDisparo { DISPARO_NO, DISPARO_GIRANDO, DISPARO_RETROCEDIENDO, DISPARO_ESPERANDO };
FaseDisparo faseDisparo = DISPARO_NO;
String disparoModo = "";
unsigned long disparoMs1 = 0, disparoMs2 = 0;   // tiempo del rele 1 y del rele 2
long disparoPulsosRetroceso = 0;                // 0 = sin retroceso
bool disparoDerecha = true;                     // sentido del avance
unsigned long disparoEsperaInicio = 0;          // instante (millis) en que paro el giro

// Secuencia automatica al detectar pienso (se guarda en la memoria del ESP32)
Preferences ajustes;
bool autoActivo = false;
String autoModo = "1";                          // mismos modos que el disparo
unsigned long autoMs1 = RELE_TIEMPO_S * 1000UL, autoMs2 = RELE_TIEMPO_S * 1000UL;
unsigned long piensoDetecciones = 0;            // detecciones desde el arranque

// Modo de la WiFi. MODO_APAGADO (sin WiFi) solo se pide con /wifi?modo=apagado
// por el puerto serie, para pruebas.
enum ModoWifi { MODO_AP, MODO_BUSCANDO, MODO_HOST, MODO_APAGADO };
ModoWifi modoWifi = MODO_AP;
volatile int wifiPedido = -1;   // modo pedido con /wifi, lo aplica loop() (-1 = ninguno)

hw_timer_t* temporizadorPasos = NULL;   // temporizador hardware de los pulsos
portMUX_TYPE candadoMotor = portMUX_INITIALIZER_UNLOCKED;  // evita que el
                         // temporizador y la web toquen el estado a la vez


// ---------------------------------------------------------------------
//  DEPURACION
// ---------------------------------------------------------------------

// Escribe una linea en el Monitor Serie con la hora desde el arranque:
// [   123.456] texto
void aviso(const String& texto) {
  unsigned long ms = millis();
  char hora[16];
  snprintf(hora, sizeof(hora), "[%6lu.%03lu] ", ms / 1000, ms % 1000);
  Serial.print(hora);
  Serial.println(texto);
}

// Como aviso(), pero solo si DEPURACION esta activada.
void depurar(const String& texto) {
  if (DEPURACION) aviso(texto);
}

// MAC en formato AA:BB:CC:DD:EE:FF
String textoMac(const uint8_t* mac) {
  char texto[18];
  snprintf(texto, sizeof(texto), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(texto);
}

// Avisos de la WiFi: quien se conecta a la red propia y como va la conexion
// con el router. Lo llama el ESP32 en cada evento WiFi.
void eventoWifi(arduino_event_id_t evento, arduino_event_info_t info) {
  switch (evento) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      depurar(String("[wifi] AP: se conecta un dispositivo, MAC ") +
              textoMac(info.wifi_ap_staconnected.mac) +
              " (conectados: " + String(WiFi.softAPgetStationNum()) + ")");
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      depurar(String("[wifi] AP: IP asignada ") +
              IPAddress(info.wifi_ap_staipassigned.ip.addr).toString());
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      depurar(String("[wifi] AP: se desconecta MAC ") +
              textoMac(info.wifi_ap_stadisconnected.mac) +
              " (conectados: " + String(WiFi.softAPgetStationNum()) + ")");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      depurar(String("[wifi] host: asociado a la red, canal ") +
              String(info.wifi_sta_connected.channel));
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      depurar(String("[wifi] host: IP ") + IPAddress(info.got_ip.ip_info.ip.addr).toString());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      uint8_t motivo = info.wifi_sta_disconnected.reason;
      depurar(String("[wifi] host: desconectado, motivo ") + String(motivo) + " (" +
              WiFi.disconnectReasonName((wifi_err_reason_t)motivo) + ")");
      break;
    }
    default:
      break;
  }
}

// IP de quien hace la peticion. El servidor escucha en IPv6 e IPv4 a la vez,
// asi que una IP v4 llega como ::ffff:a.b.c.d y se saca de los 4 ultimos bytes.
String ipCliente(httpd_req_t* req) {
  struct sockaddr_storage dir;
  socklen_t largo = sizeof(dir);
  if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr*)&dir, &largo) != 0) return "?";
  if (dir.ss_family == AF_INET) {
    return IPAddress(((struct sockaddr_in*)&dir)->sin_addr.s_addr).toString();
  }
  if (dir.ss_family == AF_INET6) {
    const uint8_t* b = ((struct sockaddr_in6*)&dir)->sin6_addr.s6_addr;
    return IPAddress(b[12], b[13], b[14], b[15]).toString();
  }
  return "?";
}

// Valor de un parametro de la consulta (lo que va tras '?' en
// /mover?vueltas=1.5&sentido=der). Vacio si no esta.
String argumento(const char* consulta, const char* nombre) {
  char valor[32];
  if (httpd_query_key_value(consulta, nombre, valor, sizeof(valor)) != ESP_OK) return "";
  return String(valor);
}

// Contadores del diagnostico (ver DIAGNOSTICO_MS y /diagnostico). Los
// escriben la tarea del servidor web, el temporizador de los pulsos y loop(),
// y los pone a cero reiniciarContadores().
volatile unsigned long webPeticiones = 0;     // respuestas enviadas
volatile unsigned long webFallos = 0;         // respuestas que no se pudieron enviar
volatile unsigned long webEnvioMaxMs = 0;     // envio mas lento, en ms
volatile unsigned long webAtencionMaxMs = 0;  // peticion mas lenta, de principio a fin, en ms
volatile int64_t pasoAnteriorUs = 0;          // instante del cambio de STEP anterior (0 = primero)
volatile unsigned long pasoDesvioMaxUs = 0;   // mayor desvio entre dos cambios de STEP
                                              // respecto al medio periodo, en us
volatile unsigned long pasoIrregulares = 0;   // cambios de STEP desviados mas del 20%
volatile unsigned long pasoCambios = 0;       // cambios de STEP medidos
volatile unsigned long bucleMaxUs = 0;        // pasada de loop() mas larga, en us

void reiniciarContadores() {
  webPeticiones = 0;
  webFallos = 0;
  webEnvioMaxMs = 0;
  webAtencionMaxMs = 0;
  pasoDesvioMaxUs = 0;
  pasoIrregulares = 0;
  pasoCambios = 0;
  bucleMaxUs = 0;
}

// Envia la respuesta de la web y la anota: IP de quien pregunta, orden y
// codigo de respuesta (con el texto, si es corto).
esp_err_t responder(httpd_req_t* req, int codigo, const char* tipo, const String& cuerpo) {
  const char* estado = "200 OK";
  if (codigo == 400) estado = "400 Bad Request";
  else if (codigo == 404) estado = "404 Not Found";
  else if (codigo == 409) estado = "409 Conflict";
  httpd_resp_set_status(req, estado);
  httpd_resp_set_type(req, tipo);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  unsigned long inicioEnvio = millis();
  esp_err_t resultado = httpd_resp_send(req, cuerpo.c_str(), cuerpo.length());
  unsigned long msEnvio = millis() - inicioEnvio;
  webPeticiones++;
  if (resultado != ESP_OK) webFallos++;
  if (msEnvio > webEnvioMaxMs) webEnvioMaxMs = msEnvio;
  if (msEnvio >= 1000) {
    aviso(String("[web] envio lento: ") + req->uri + " " + msEnvio + " ms a " + ipCliente(req));
  }

  bool esEstado = (strncmp(req->uri, "/estado", 7) == 0);
  if (!DEPURACION || (esEstado && !DEPURAR_ESTADO && codigo == 200)) return resultado;

  String linea = "[web] ";
  linea += ipCliente(req);
  linea += (req->method == HTTP_GET) ? " GET " : " otro ";
  linea += req->uri;
  linea += " -> ";
  linea += codigo;
  if (String(tipo).startsWith("text/plain")) {
    linea += " ";
    linea += cuerpo.length() > 70 ? cuerpo.substring(0, 70) + "..." : cuerpo;
  }
  if (resultado != ESP_OK) linea += " (no se pudo enviar: el navegador ya habia cerrado)";
  depurar(linea);
  return resultado;
}


// ---------------------------------------------------------------------
//  MOTOR
// ---------------------------------------------------------------------

// Interrupcion del temporizador, cada pulsoUs microsegundos. Alterna STEP:
// un pulso es ALTO durante pulsoUs y BAJO durante pulsoUs, y cuenta como dado
// al bajar STEP. Corre en el nucleo 1 (el de loop), lejos de la WiFi, que
// trabaja en el nucleo 0: asi los pulsos salen a su hora aunque la WiFi este
// ocupada. Escribe STEP directamente en el registro (gpio_ll_set_level), que
// es mucho mas rapido que digitalWrite.
void IRAM_ATTR darPaso() {
  int64_t ahora = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&candadoMotor);
  if (pulsosPendientes > 0) {
    // Diagnostico: cuanto se aparta este cambio de STEP del medio periodo
    if (pasoAnteriorUs != 0) {
      long desvio = (long)(ahora - pasoAnteriorUs) - (long)pulsoUs;
      if (desvio < 0) desvio = -desvio;
      if ((unsigned long)desvio > pasoDesvioMaxUs) pasoDesvioMaxUs = desvio;
      if ((unsigned long)desvio * 5 > pulsoUs) pasoIrregulares++;
      pasoCambios++;
    }
    pasoAnteriorUs = ahora;
    if (!stepEnAlto) {
      gpio_ll_set_level(&GPIO, STEP_PIN, 1);
      stepEnAlto = true;
    } else {
      gpio_ll_set_level(&GPIO, STEP_PIN, 0);
      stepEnAlto = false;
      pulsosPendientes = pulsosPendientes - 1;
    }
  }
  portEXIT_CRITICAL_ISR(&candadoMotor);
}

// Arranca el temporizador de los pulsos con el medio periodo pulsoUs, o lo para.
void arrancarTemporizador() {
  timerWrite(temporizadorPasos, 0);
  timerAlarm(temporizadorPasos, pulsoUs, true, 0);
  timerStart(temporizadorPasos);
  temporizadorEnMarcha = true;
}

void pararTemporizador() {
  if (temporizadorEnMarcha) timerStop(temporizadorPasos);
  temporizadorEnMarcha = false;
}

// Da corriente a las bobinas (driver habilitado por EN).
void activarDriver() {
  if (driverActivo) return;
  digitalWrite(ENABLE_PIN, ENABLE_ACTIVO);
  driverActivo = true;
  delay(ENABLE_ESPERA_MS);   // deja que el eje se asiente antes de mover
}

// Corta la corriente de las bobinas. El eje queda libre.
void desactivarDriver() {
  digitalWrite(ENABLE_PIN, !ENABLE_ACTIVO);
  driverActivo = false;
  aviso("[motor] en reposo: sin corriente");
}

// Prepara un movimiento de 'pulsos' pulsos en el sentido indicado
// (true = derecha, false = izquierda) y arranca el temporizador.
void moverMotor(long pulsos, bool direccion) {
  if (pulsos <= 0) return;
  pararTemporizador();
  activarDriver();

  // 1. Configurar DIR
  digitalWrite(DIR_PIN, direccion ? DIR_DERECHA : !DIR_DERECHA);
  delayMicroseconds(DIR_SETUP_US);

  // 2. Encargar el numero exacto de pulsos y arrancar el temporizador
  sentidoDerecha = direccion;
  portENTER_CRITICAL(&candadoMotor);
  digitalWrite(STEP_PIN, LOW);
  stepEnAlto = false;
  pulsosPendientes = pulsos;
  pasoAnteriorUs = 0;
  portEXIT_CRITICAL(&candadoMotor);
  arrancarTemporizador();
}

// Cambia la velocidad a 'rpm' vueltas por minuto (de RPM_MIN a RPM_MAX). Si
// el motor esta girando, el cambio vale ya para los pulsos que faltan.
void cambiarVelocidad(int rpm) {
  rpmActual = rpm;
  // medio periodo = 60 s / (rpm * pulsos por vuelta) / 2
  pulsoUs = 30000000UL / ((unsigned long)rpm * PULSOS_POR_VUELTA);
  if (pulsoUs < 50) pulsoUs = 50;     // mas rapido, la interrupcion ocuparia el procesador
  if (temporizadorEnMarcha) {
    pararTemporizador();
    pasoAnteriorUs = 0;   // el primer cambio con el periodo nuevo no cuenta como desvio
    arrancarTemporizador();
  }
}

// Detiene el motor inmediatamente.
void pararMotor() {
  pararTemporizador();
  finMovimiento = millis();
  portENTER_CRITICAL(&candadoMotor);
  pulsosPendientes = 0;
  digitalWrite(STEP_PIN, LOW);
  stepEnAlto = false;
  portEXIT_CRITICAL(&candadoMotor);
}

// Se llama en loop(): cuando el temporizador ha dado todos los pulsos, lo
// para y lo avisa por el Monitor Serie. Si DESACTIVAR_EN_REPOSO, corta la
// corriente cuando el motor lleva REPOSO_MS parado.
void comprobarFinMovimiento() {
  if (temporizadorEnMarcha && pulsosPendientes == 0) {
    pararTemporizador();
    finMovimiento = millis();
    aviso("[motor] movimiento terminado");
  }
  if (DESACTIVAR_EN_REPOSO && driverActivo && !temporizadorEnMarcha &&
      millis() - finMovimiento >= REPOSO_MS) {
    desactivarDriver();
  }
}


// ---------------------------------------------------------------------
//  RELES
// ---------------------------------------------------------------------

// Enciende el rele i (0 = rele 1, 1 = rele 2) durante 'ms' milisegundos. Si
// ya estaba encendido, vuelve a empezar a contar con el tiempo nuevo.
void encenderRele(int i, unsigned long ms) {
  digitalWrite(reles[i].pin, RELE_ACTIVO);
  reles[i].encendido = true;
  reles[i].inicio = millis();
  reles[i].duracionMs = ms;
}

void apagarRele(int i) {
  digitalWrite(reles[i].pin, !RELE_ACTIVO);
  reles[i].encendido = false;
}

// Milisegundos que le quedan encendido al rele i (0 si esta apagado).
unsigned long releRestante(int i) {
  if (!reles[i].encendido) return 0;
  unsigned long transcurrido = millis() - reles[i].inicio;
  return transcurrido < reles[i].duracionMs ? reles[i].duracionMs - transcurrido : 0;
}

// Cancela la activacion en serie pendiente (el segundo rele ya no se enciende).
void cancelarSerie() {
  serieTras = -1;
  serieSiguiente = -1;
}

// Activacion en serie: enciende el rele 'primero' durante 'msPrimero' y,
// cuando se apaga, el rele 'segundo' durante 'msSegundo'.
void encenderSerie(int primero, unsigned long msPrimero, int segundo, unsigned long msSegundo) {
  apagarRele(segundo);              // el segundo espera apagado su turno
  encenderRele(primero, msPrimero);
  serieTras = primero;
  serieSiguiente = segundo;
  serieMs = msSegundo;
}

// Enciende los reles segun el modo: "1", "2", "12" (serie 1 y luego 2) o
// "21" (serie 2 y luego 1). ms1 y ms2 son los tiempos del rele 1 y del rele 2.
void encenderModo(const String& modo, unsigned long ms1, unsigned long ms2) {
  cancelarSerie();
  if (modo == "1") encenderRele(0, ms1);
  else if (modo == "2") encenderRele(1, ms2);
  else if (modo == "12") encenderSerie(0, ms1, 1, ms2);
  else if (modo == "21") encenderSerie(1, ms2, 0, ms1);
}

// Cancela el disparo pendiente (si los reles aun no se han encendido).
void cancelarDisparo() {
  if (faseDisparo != DISPARO_NO) aviso("[disparo] cancelado");
  faseDisparo = DISPARO_NO;
}

// Se llama en loop(), despues de comprobarFinMovimiento(): cuando el avance
// del disparo termina, retrocede (si hay retroceso); al acabar, espera
// DISPARO_ESPERA_MS y enciende los reles. No bloquea.
void comprobarDisparo() {
  bool giroAcabado = !temporizadorEnMarcha && pulsosPendientes == 0;
  if (faseDisparo == DISPARO_GIRANDO && giroAcabado && disparoPulsosRetroceso > 0) {
    faseDisparo = DISPARO_RETROCEDIENDO;
    moverMotor(disparoPulsosRetroceso, !disparoDerecha);
    aviso("[disparo] avance terminado: retrocedo " + String(disparoPulsosRetroceso) + " pulsos");
  } else if ((faseDisparo == DISPARO_GIRANDO || faseDisparo == DISPARO_RETROCEDIENDO) && giroAcabado) {
    faseDisparo = DISPARO_ESPERANDO;
    disparoEsperaInicio = millis();
    aviso("[disparo] giro terminado: espero " + String(DISPARO_ESPERA_MS) + " ms");
  }
  if (faseDisparo == DISPARO_ESPERANDO && millis() - disparoEsperaInicio >= DISPARO_ESPERA_MS) {
    faseDisparo = DISPARO_NO;
    encenderModo(disparoModo, disparoMs1, disparoMs2);
    aviso("[disparo] disparo: reles en modo " + disparoModo);
  }
}

// Pienso detectado: si la secuencia automatica esta activada, enciende los
// reles segun el modo guardado. De momento solo la llama el boton "Simular
// deteccion"; cuando haya sensor, la llamara loop(). Devuelve true si disparo.
bool piensoDetectado(const char* origen) {
  piensoDetecciones++;
  if (!autoActivo) {
    aviso(String("[pienso] detectado (") + origen + "): secuencia automatica desactivada");
    return false;
  }
  encenderModo(autoModo, autoMs1, autoMs2);
  aviso(String("[pienso] detectado (") + origen + "): reles en modo " + autoModo);
  return true;
}

// Guarda la secuencia automatica en la memoria del ESP32 (no se borra al apagar).
void guardarAutomatico() {
  ajustes.putBool("auto_activo", autoActivo);
  ajustes.putString("auto_modo", autoModo);
  ajustes.putULong("auto_ms1", autoMs1);
  ajustes.putULong("auto_ms2", autoMs2);
}

// Lee la secuencia automatica guardada (en setup).
void leerAutomatico() {
  autoActivo = ajustes.getBool("auto_activo", false);
  autoModo = ajustes.getString("auto_modo", "1");
  if (autoModo != "1" && autoModo != "2" && autoModo != "12" && autoModo != "21") autoModo = "1";
  autoMs1 = ajustes.getULong("auto_ms1", RELE_TIEMPO_S * 1000UL);
  autoMs2 = ajustes.getULong("auto_ms2", RELE_TIEMPO_S * 1000UL);
}

// Se llama en loop(): apaga cada rele cuando ha pasado su tiempo y, si era el
// primero de una serie, enciende el siguiente. No bloquea.
void comprobarReles() {
  for (int i = 0; i < NUM_RELES; i++) {
    if (reles[i].encendido && millis() - reles[i].inicio >= reles[i].duracionMs) {
      apagarRele(i);
      aviso(String("[rele] rele ") + (i + 1) + " apagado");
      if (serieTras == i) {
        int siguiente = serieSiguiente;
        cancelarSerie();
        encenderRele(siguiente, serieMs);
        aviso(String("[rele] serie: enciendo el rele ") + (siguiente + 1) + " " +
              String(serieMs / 1000.0, 1) + " s");
      }
    }
  }
}


// ---------------------------------------------------------------------
//  PAGINA WEB
// ---------------------------------------------------------------------

// %PPV%, %RELE_S%, %ESPERA_S%, %RPM%, %RPM_MIN%, %RPM_MAX% y %GRADOS_PASO%
// se sustituyen al enviar la pagina (paginaPrincipal).
const char PAGINA[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Control motor paso a paso</title>
<style>
  body { font-family: sans-serif; max-width: 720px; margin: 0 auto; padding: 16px;
         background: #f2f2f2; color: #222; display: grid;
         grid-template-columns: minmax(0, 1fr); gap: 12px; align-items: start; }
  h1 { grid-column: 1 / -1; }
  @media (max-width: 650px) { body { grid-template-columns: 1fr; } }
  h1 { font-size: 1.4em; text-align: center; }
  .caja { background: #fff; border-radius: 8px; padding: 16px; min-width: 0; }
  label { display: block; margin-bottom: 6px; }
  input { width: 100%; font-size: 1.3em; padding: 8px; box-sizing: border-box; }
  .fila { display: flex; gap: 10px; }
  button { flex: 1; width: 100%; font-size: 1.05em; padding: 14px 6px; margin-top: 10px;
           border: 0; border-radius: 6px; background: #1565c0; color: #fff; }
  button:active { background: #0d47a1; }
  button.parar { background: #c62828; }
  button.rele { background: #2e7d32; }
  button.serie { background: #00695c; }
  button.disparo { background: #e65100; font-weight: bold; }
  .opciones { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
  .opciones label { flex: 1 1 40%; margin: 0; }
  .opciones input { position: absolute; opacity: 0; width: 0; }
  .opciones span { display: block; text-align: center; padding: 10px 4px; border-radius: 6px;
                   border: 2px solid #bbb; background: #fafafa; cursor: pointer; }
  .opciones input:checked + span { border-color: #e65100; background: #fff3e0; font-weight: bold; }
  .opciones input:focus-visible + span { outline: 2px solid #1565c0; }
  .titulo-opciones { margin-top: 14px; font-size: 0.95em; color: #555; }
  .rele-fila { display: flex; gap: 10px; align-items: flex-end; }
  .rele-fila > div { flex: 1; }
  .rele-fila button { margin-top: 0; }
  .deslizador { margin-bottom: 14px; }
  .deslizador label { display: flex; justify-content: space-between; }
  .deslizador b { font-size: 1.15em; }
  input[type=range] { padding: 0; height: 32px; accent-color: #1565c0; }
  .limites { display: flex; justify-content: space-between; font-size: 0.8em; color: #777; }
  .nota { font-size: 0.85em; color: #666; margin-top: 8px; }
  .sensor { display: flex; align-items: center; gap: 10px; }
  .piloto { width: 18px; height: 18px; border-radius: 50%; background: #bbb; flex: none; }
  .piloto.activo { background: #2e7d32; box-shadow: 0 0 8px #2e7d32; }
  .interruptor { display: flex; align-items: center; gap: 10px; margin: 12px 0 0; cursor: pointer; }
  .interruptor input { width: 22px; height: 22px; margin: 0; }
  .opciones.apagadas { opacity: 0.45; }
  button.sensor-boton { background: #6a1b9a; }
  #estado { font-weight: bold; }
  .dato-motor { padding-bottom: 12px; margin-bottom: 12px; border-bottom: 1px solid #eee; }
  .columna-derecha { display: contents; }
  .pilotos-entradas { display: flex; flex-wrap: wrap; gap: 16px; margin: 12px 0; }
  .piloto-entrada { display: flex; align-items: center; gap: 8px; font-weight: 600; }
  .piloto-entrada .piloto { width: 22px; height: 22px; }
  .caja-estado { grid-column: 1 / -1; }
  @media (max-width: 650px) { .caja-estado { grid-column: auto; } }
</style>
</head>
<body>
  <h1>Control motor paso a paso</h1>

  <div class="caja">
    <div class="dato-motor">Pulsos por vuelta: <b>%PPV%</b></div>
    <div class="deslizador">
      <label for="rpm"><span>Velocidad</span><b><span id="rpmTexto">%RPM%</span> rpm</b></label>
      <input id="rpm" type="range" min="%RPM_MIN%" max="%RPM_MAX%" step="5" value="%RPM%"
             oninput="verVelocidad()" onchange="enviarVelocidad()">
      <div class="limites"><span>%RPM_MIN%</span><span>%RPM_MAX%</span></div>
    </div>
    <div class="deslizador">
      <label for="avance"><span>Ángulo de avance</span><b id="avanceTexto">-</b></label>
      <input id="avance" type="range" min="0" max="360" step="1" value="90" oninput="verAngulos()">
      <div class="limites"><span>0°</span><span>360°</span></div>
    </div>
    <div class="deslizador">
      <label for="retroceso"><span>Ángulo de retroceso</span><b id="retrocesoTexto">-</b></label>
      <input id="retroceso" type="range" min="0" max="360" step="1" value="0" oninput="verAngulos()">
      <div class="limites"><span>0°</span><span>360°</span></div>
    </div>
    <div class="fila">
      <button onclick="girar('izq')">Retroceder</button>
      <button onclick="girar('der')">Avanzar</button>
    </div>
    <button class="parar" onclick="parar(5)">Parar</button>
    <div class="nota">Avanzar gira a la derecha y retroceder a la izquierda. Cada paso son %GRADOS_PASO%°.</div>
  </div>

  <div class="columna-derecha">
  <div class="caja">
    <div class="rele-fila">
      <div>
        <label for="segundos1">Relé 1 (segundos)</label>
        <input id="segundos1" type="number" min="0" step="any" value="%RELE_S%" inputmode="decimal" onchange="guardarAutomatico()">
      </div>
      <button class="rele" onclick="activarRele('1')">Activar relé 1</button>
    </div>
    <div class="rele-fila" style="margin-top: 12px">
      <div>
        <label for="segundos2">Relé 2 (segundos)</label>
        <input id="segundos2" type="number" min="0" step="any" value="%RELE_S%" inputmode="decimal" onchange="guardarAutomatico()">
      </div>
      <button class="rele" onclick="activarRele('2')">Activar relé 2</button>
    </div>
    <div class="fila">
      <button class="serie" onclick="activarSerie('12')">Relé 1 y luego relé 2</button>
      <button class="serie" onclick="activarSerie('21')">Relé 2 y luego relé 1</button>
    </div>
  </div>

  <div class="caja">
    <button class="disparo" style="margin-top: 0" onclick="disparar()">Realizar disparo</button>
    <div class="titulo-opciones">Avanza el ángulo de avance, retrocede el de retroceso, espera %ESPERA_S% s y dispara con:</div>
    <div class="opciones">
      <label><input type="radio" name="modo" value="1" checked><span>Relé 1</span></label>
      <label><input type="radio" name="modo" value="2"><span>Relé 2</span></label>
      <label><input type="radio" name="modo" value="12"><span>Relé 1 y luego 2</span></label>
      <label><input type="radio" name="modo" value="21"><span>Relé 2 y luego 1</span></label>
    </div>
  </div>

  <div class="caja">
    <div class="sensor">
      <span class="piloto" id="piloto"></span>
      <div><b>Sensor de pienso</b><br><span id="sensorTexto">fotodetectores conectados</span></div>
    </div>
    <div class="pilotos-entradas" aria-label="Estado de los fotodetectores">
      <div class="piloto-entrada"><span class="piloto" id="foto1" role="img" aria-label="D15 sin detección"></span><span>D15</span></div>
      <div class="piloto-entrada"><span class="piloto" id="foto2" role="img" aria-label="D16 sin detección"></span><span>D16</span></div>
    </div>
    <label class="interruptor"><input type="checkbox" id="autoActivo" onchange="guardarAutomatico()">
      Disparo automático al detectar pienso</label>
    <div class="titulo-opciones">Secuencia que se lanza sola, con los tiempos de los relés de arriba:</div>
    <div class="opciones" id="autoOpciones">
      <label><input type="radio" name="autoModo" value="1" checked onchange="guardarAutomatico()"><span>Relé 1</span></label>
      <label><input type="radio" name="autoModo" value="2" onchange="guardarAutomatico()"><span>Relé 2</span></label>
      <label><input type="radio" name="autoModo" value="12" onchange="guardarAutomatico()"><span>Relé 1 y luego 2</span></label>
      <label><input type="radio" name="autoModo" value="21" onchange="guardarAutomatico()"><span>Relé 2 y luego 1</span></label>
    </div>
    <button class="sensor-boton" onclick="orden('/pienso')">Simular detección</button>
    <div class="nota">La selección se guarda en el ESP32 y se mantiene al apagarlo.</div>
  </div>

  </div> <!-- columna derecha -->

  <div class="caja caja-estado">
    Motor: <span id="estado">-</span><br>
    Disparo: <span id="disparo">-</span><br>
    Relé 1: <span id="rele1">-</span><br>
    Relé 2: <span id="rele2">-</span><br>
    <span id="serie"></span>
    <span id="mensaje"></span>
  </div>

<script>
  // fetch que se rinde a los 'ms' milisegundos. Sin esto, una peticion que
  // no llega se queda colgada y las siguientes esperan detras de ella.
  function pedir(url, ms) {
    var control = new AbortController();
    var reloj = setTimeout(() => control.abort(), ms);
    return fetch(url, { signal: control.signal }).finally(() => clearTimeout(reloj));
  }

  // Envia una orden al ESP32 y muestra su respuesta
  function orden(url) {
    pedir(url, 5000)
      .then(r => r.text())
      .then(t => { document.getElementById('mensaje').textContent = t; })
      .catch(() => { document.getElementById('mensaje').textContent = 'Sin respuesta del ESP32'; });
  }

  var PPV = %PPV%;

  // Velocidad: el numero cambia al mover el deslizador y la orden se envia al soltarlo
  function verVelocidad() {
    document.getElementById('rpmTexto').textContent = document.getElementById('rpm').value;
  }
  function enviarVelocidad() {
    orden('/velocidad?rpm=' + document.getElementById('rpm').value);
  }

  // Angulos en grados (0 a 360) y su equivalente en vueltas y pasos
  function grados(id) { return parseInt(document.getElementById(id).value, 10); }
  function vueltasDe(g) { return (g / 360).toFixed(4); }
  function verAngulos() {
    ['avance', 'retroceso'].forEach(id => {
      var g = grados(id);
      document.getElementById(id + 'Texto').textContent = g + '° (' + Math.round(g / 360 * PPV) + ' pasos)';
    });
    try { localStorage.setItem('angulos', JSON.stringify([grados('avance'), grados('retroceso')])); } catch (e) {}
  }
  try {
    var guardados = JSON.parse(localStorage.getItem('angulos'));
    if (guardados) { document.getElementById('avance').value = guardados[0]; document.getElementById('retroceso').value = guardados[1]; }
  } catch (e) {}
  verAngulos();

  // 'der' avanza el angulo de avance; 'izq' retrocede el angulo de retroceso
  function girar(sentido) {
    var g = grados(sentido === 'der' ? 'avance' : 'retroceso');
    if (Math.round(g / 360 * PPV) < 1) {
      document.getElementById('mensaje').textContent = 'El ángulo es 0: no hay nada que girar.';
      return;
    }
    orden('/mover?vueltas=' + vueltasDe(g) + '&sentido=' + sentido);
  }

  // Orden de parar. Si no llega al ESP32, se reintenta hasta 'intentos' veces.
  function parar(intentos) {
    pedir('/parar', 2000)
      .then(r => r.text())
      .then(t => { document.getElementById('mensaje').textContent = t; })
      .catch(() => {
        if (intentos > 1) setTimeout(() => parar(intentos - 1), 200);
        else document.getElementById('mensaje').textContent = 'No se pudo enviar Parar';
      });
  }

  // Enciende el rele 1 o el 2 con los segundos de su campo
  function segundosRele(n) {
    return encodeURIComponent(document.getElementById('segundos' + n).value.replace(',', '.'));
  }
  function activarRele(n) {
    orden('/rele?n=' + n + '&segundos=' + segundosRele(n));
  }

  // Disparo: avanza el angulo de avance, retrocede el de retroceso, espera y
  // enciende los reles en el modo elegido, cada uno con los segundos de su campo
  function disparar() {
    if (Math.round(grados('avance') / 360 * PPV) < 1) {
      document.getElementById('mensaje').textContent = 'El ángulo de avance es 0: el disparo necesita avanzar.';
      return;
    }
    var modo = document.querySelector('input[name=modo]:checked').value;
    orden('/disparo?vueltas=' + vueltasDe(grados('avance')) + '&sentido=der' +
          '&retroceso=' + vueltasDe(grados('retroceso')) + '&modo=' + modo +
          '&segundos1=' + segundosRele(1) + '&segundos2=' + segundosRele(2));
  }

  // Secuencia automatica del sensor de pienso. Se guarda en el ESP32 al
  // cambiar la casilla, la opcion o los tiempos de los reles (no antes de
  // haber leido lo guardado, para no pisarlo al abrir la pagina).
  var automaticoLeido = false;
  function verAutomatico() {
    document.getElementById('autoOpciones').classList.toggle('apagadas', !document.getElementById('autoActivo').checked);
  }
  function guardarAutomatico() {
    verAutomatico();
    if (!automaticoLeido) return;
    var modo = document.querySelector('input[name=autoModo]:checked').value;
    orden('/automatico?activo=' + (document.getElementById('autoActivo').checked ? 1 : 0) + '&modo=' + modo +
          '&segundos1=' + segundosRele(1) + '&segundos2=' + segundosRele(2));
  }

  // Activacion en serie: '12' = rele 1 y, al apagarse, rele 2; '21' = al reves.
  // Cada rele con los segundos de su campo.
  function activarSerie(ordenSerie) {
    orden('/serie?orden=' + ordenSerie + '&segundos1=' + segundosRele(1) + '&segundos2=' + segundosRele(2));
  }

  // Texto del estado de un rele
  function textoRele(encendido, ms) {
    return encendido ? 'encendido, quedan ' + (ms / 1000).toFixed(1) + ' s' : 'apagado';
  }

  // Consulta el estado del motor cada segundo. La siguiente consulta sale un
  // segundo DESPUES de que llegue (o se abandone) la anterior, nunca a la vez:
  // el servidor del ESP32 atiende las peticiones de una en una y, si se
  // acumulan, las ordenes de los botones esperan detras de todas ellas.
  // Con la pagina oculta (otra pestana, pantalla apagada) no pregunta.
  var detecciones = null;
  var siguienteEstado = null;
  function programarEstado(ms) {
    clearTimeout(siguienteEstado);
    siguienteEstado = setTimeout(leerEstado, ms);
  }
  document.addEventListener('visibilitychange', () => { if (!document.hidden) programarEstado(0); });
  var estadoEnCurso = false;
  function leerEstado() {
    if (document.hidden || estadoEnCurso) return;
    estadoEnCurso = true;
    pedir('/estado', 3000)
      .then(r => r.json())
      .then(e => {
        document.getElementById('estado').textContent = e.moviendo
          ? 'girando a la ' + e.sentido + ', quedan ' + e.pulsos_pendientes + ' pulsos'
          : (e.con_corriente ? 'parado, con corriente' : 'parado, sin corriente');
        var modos = { '1': 'relé 1', '2': 'relé 2', '12': 'relé 1 y luego 2', '21': 'relé 2 y luego 1' };
        document.getElementById('disparo').textContent =
          e.disparo === 'girando' ? 'avanzando, después dispara con ' + modos[e.disparo_modo]
          : e.disparo === 'retrocediendo' ? 'retrocediendo, después dispara con ' + modos[e.disparo_modo]
          : e.disparo === 'esperando' ? 'esperando para disparar con ' + modos[e.disparo_modo]
          : 'listo';
        // La primera vez, los controles toman lo que tiene el ESP32
        if (!automaticoLeido) {
          document.getElementById('rpm').value = e.rpm;
          verVelocidad();
          document.getElementById('autoActivo').checked = e.auto_activo;
          document.querySelector('input[name=autoModo][value="' + e.auto_modo + '"]').checked = true;
          verAutomatico();
          automaticoLeido = true;
        }
        // Pilotos individuales: encendidos mientras la entrada detecta (LOW).
        ['foto1', 'foto2'].forEach((id, i) => {
          const detectado = i === 0 ? e.foto_d15 : e.foto_d16;
          const pilotoEntrada = document.getElementById(id);
          pilotoEntrada.classList.toggle('activo', !!detectado);
          pilotoEntrada.setAttribute('aria-label', (i === 0 ? 'D15' : 'D16') +
            (detectado ? ': detección activa' : ': sin detección'));
          pilotoEntrada.title = detectado ? 'Detectado (LOW)' : 'Libre (HIGH)';
        });
        // Piloto del sensor: se enciende 2 s con cada deteccion nueva
        document.getElementById('sensorTexto').textContent = 'fotodetectores conectados' +
          (e.pienso_detecciones ? ', ' + e.pienso_detecciones + ' detecciones de prueba' : '');
        if (detecciones !== null && e.pienso_detecciones > detecciones) {
          var piloto = document.getElementById('piloto');
          piloto.classList.add('activo');
          setTimeout(() => piloto.classList.remove('activo'), 2000);
        }
        detecciones = e.pienso_detecciones;
        document.getElementById('rele1').textContent = textoRele(e.rele, e.rele_ms_restantes);
        document.getElementById('rele2').textContent = textoRele(e.rele2, e.rele2_ms_restantes);
        document.getElementById('serie').innerHTML = e.serie_siguiente
          ? 'En serie: después se enciende el relé ' + e.serie_siguiente + ' (' + (e.serie_ms / 1000).toFixed(1) + ' s)<br>'
          : '';
      })
      .catch(() => { document.getElementById('estado').textContent = 'sin conexión'; })
      .finally(() => { estadoEnCurso = false; programarEstado(1000); });
  }
  leerEstado();
</script>
</body>
</html>
)rawliteral";


// ---------------------------------------------------------------------
//  ORDENES (WEB Y PUERTO SERIE)
// ---------------------------------------------------------------------

// Las ordenes corren en la tarea del servidor web (peticiones de la web) o en
// loop() (ordenes por el puerto serie). Toman candadoEstado solo mientras leen
// o cambian el estado del motor y de los reles, y lo sueltan ANTES de enviar
// la respuesta: asi un envio lento a un movil con mala cobertura nunca frena
// el bucle principal ni el motor.

// Pagina de control ya preparada (la hace prepararPagina en setup). Antes se
// copiaba y se rellenaba en cada visita: 15 KB de memoria y siete pasadas
// por el texto cada vez que alguien abria o recargaba la pagina.
String paginaLista;

// Sustituye %PPV%, %RELE_S%... en PAGINA. %RPM% toma RPM_INICIAL: al abrirse,
// la pagina pide /estado y pone en el deslizador la velocidad que tenga el
// motor en ese momento.
void prepararPagina() {
  unsigned long inicio = millis();
  paginaLista = PAGINA;
  paginaLista.replace("%PPV%", String(PULSOS_POR_VUELTA));
  paginaLista.replace("%RELE_S%", String(RELE_TIEMPO_S));
  paginaLista.replace("%ESPERA_S%", String(DISPARO_ESPERA_MS / 1000.0, DISPARO_ESPERA_MS % 1000 ? 1 : 0));
  paginaLista.replace("%RPM%", String(RPM_INICIAL));
  paginaLista.replace("%RPM_MIN%", String(RPM_MIN));
  paginaLista.replace("%RPM_MAX%", String(RPM_MAX));
  String gradosPaso = String(360.0 / PULSOS_POR_VUELTA, 3);   // 1.800 -> 1,8
  while (gradosPaso.endsWith("0")) gradosPaso.remove(gradosPaso.length() - 1);
  if (gradosPaso.endsWith(".")) gradosPaso.remove(gradosPaso.length() - 1);
  gradosPaso.replace(".", ",");
  paginaLista.replace("%GRADOS_PASO%", gradosPaso);
  aviso(String("[web] pagina preparada: ") + paginaLista.length() + " bytes en " +
        (millis() - inicio) + " ms");
}

// GET /  -> pagina de control
esp_err_t paginaPrincipal(httpd_req_t* req) {
  unsigned long inicio = millis();
  esp_err_t resultado = responder(req, 200, "text/html; charset=utf-8", paginaLista);
  unsigned long ms = millis() - inicio;
  if (ms > webAtencionMaxMs) webAtencionMaxMs = ms;
  return resultado;
}

// Lee vueltas y sentido de la peticion y los pasa a pulsos. Devuelve "" si
// son validos o el texto del error.
String leerGiro(const char* consulta, float& vueltas, long& pulsos, bool& derecha) {
  String textoVueltas = argumento(consulta, "vueltas");
  textoVueltas.replace(",", ".");          // admite 0,5 ademas de 0.5
  textoVueltas.replace("%2C", ".");        // la coma puede llegar codificada
  vueltas = textoVueltas.toFloat();
  String sentido = argumento(consulta, "sentido");

  if (vueltas <= 0 || vueltas > VUELTAS_MAX) {
    String error = "Número de vueltas no válido (entre 0 y ";
    error += VUELTAS_MAX;
    error += ").";
    return error;
  }
  if (sentido != "der" && sentido != "izq") return "Sentido no válido.";

  // pulsos = vueltas * PULSOS_POR_VUELTA, redondeado al pulso mas cercano
  pulsos = (long)(vueltas * PULSOS_POR_VUELTA + 0.5f);
  if (pulsos < 1) return "Son menos de un pulso.";
  derecha = (sentido == "der");
  return "";
}

// GET /mover?vueltas=1.5&sentido=der  (sentido: der o izq). Cancela el
// disparo que estuviera esperando.
Respuesta ordenMover(const char* consulta) {
  float vueltas; long pulsos; bool derecha;
  String error = leerGiro(consulta, vueltas, pulsos, derecha);
  if (error != "") return Respuesta{400, TEXTO, error};

  bool ocupado;
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  ocupado = (pulsosPendientes > 0);
  if (!ocupado) {
    cancelarDisparo();
    moverMotor(pulsos, derecha);
  }
  xSemaphoreGive(candadoEstado);

  if (ocupado) {
    return Respuesta{409, TEXTO, "El motor está girando. Espera a que termine o pulsa Parar."};
  }
  String mensaje = derecha ? "Avanzando " : "Retrocediendo ";
  mensaje += String(vueltas * 360, 0);
  mensaje += derecha ? "° a la derecha (" : "° a la izquierda (";
  mensaje += pulsos;
  mensaje += " pulsos)";
  return Respuesta{200, TEXTO, mensaje};
}

// GET /parar  (para el motor y cancela el disparo si aun no ha encendido los reles)
Respuesta ordenParar(const char* consulta) {
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  cancelarDisparo();
  pararMotor();
  xSemaphoreGive(candadoEstado);
  return Respuesta{200, TEXTO, "Motor parado."};
}

// Segundos de un parametro (admite coma). Devuelve -1 si no es valido.
float segundosDe(const char* consulta, const char* nombre) {
  String texto = argumento(consulta, nombre);
  texto.replace(",", ".");
  texto.replace("%2C", ".");
  float segundos = texto.toFloat();
  return (segundos > 0 && segundos <= RELE_MAX_S) ? segundos : -1;
}

// Texto de error de un tiempo de rele fuera de rango.
String errorTiempoRele() {
  String error = "Tiempo del relé no válido (entre 0 y ";
  error += RELE_MAX_S;
  error += " segundos).";
  return error;
}

// GET /rele?n=1&segundos=1.5  -> enciende el rele 1 durante 1,5 s y luego se apaga solo
// GET /rele?n=2&segundos=3    -> lo mismo con el rele 2
// Sin 'n' es el rele 1, como en las versiones de un solo rele. Cancela la
// activacion en serie que hubiera pendiente.
Respuesta ordenRele(const char* consulta) {
  String n = argumento(consulta, "n");
  if (n == "") n = "1";
  if (n != "1" && n != "2") {
    return Respuesta{400, TEXTO, "Relé no válido: n tiene que ser 1 o 2."};
  }
  float segundos = segundosDe(consulta, "segundos");
  if (segundos < 0) return Respuesta{400, TEXTO, errorTiempoRele()};

  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  cancelarSerie();
  encenderRele(n == "1" ? 0 : 1, (unsigned long)(segundos * 1000 + 0.5f));
  xSemaphoreGive(candadoEstado);

  String mensaje = "Relé ";
  mensaje += n;
  mensaje += " encendido ";
  mensaje += String(segundos, 1);
  mensaje += " s";
  return Respuesta{200, TEXTO, mensaje};
}

// GET /serie?orden=12&segundos1=1.5&segundos2=3
//   -> activacion en serie: el rele 1 durante segundos1 y, al apagarse, el
//      rele 2 durante segundos2. Con orden=21, primero el 2 y luego el 1
//      (cada uno con su tiempo: segundos1 es siempre el del rele 1).
Respuesta ordenSerie(const char* consulta) {
  String tipo = argumento(consulta, "orden");
  if (tipo != "12" && tipo != "21") {
    return Respuesta{400, TEXTO, "Orden no válido: tiene que ser 12 o 21."};
  }
  float s1 = segundosDe(consulta, "segundos1");
  float s2 = segundosDe(consulta, "segundos2");
  if (s1 < 0 || s2 < 0) return Respuesta{400, TEXTO, errorTiempoRele()};
  unsigned long ms1 = (unsigned long)(s1 * 1000 + 0.5f), ms2 = (unsigned long)(s2 * 1000 + 0.5f);

  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  if (tipo == "12") encenderSerie(0, ms1, 1, ms2);
  else encenderSerie(1, ms2, 0, ms1);
  xSemaphoreGive(candadoEstado);

  String mensaje = "En serie: relé ";
  if (tipo == "12") {
    mensaje += "1 " + String(s1, 1) + " s y luego relé 2 " + String(s2, 1) + " s";
  } else {
    mensaje += "2 " + String(s2, 1) + " s y luego relé 1 " + String(s1, 1) + " s";
  }
  return Respuesta{200, TEXTO, mensaje};
}

// GET /velocidad?rpm=120  -> cambia la velocidad (de RPM_MIN a RPM_MAX vueltas
// por minuto). Vale tambien para el giro en curso.
Respuesta ordenVelocidad(const char* consulta) {
  int rpm = argumento(consulta, "rpm").toInt();
  if (rpm < RPM_MIN || rpm > RPM_MAX) {
    String error = "Velocidad no válida (entre ";
    error += RPM_MIN;
    error += " y ";
    error += RPM_MAX;
    error += " rpm).";
    return Respuesta{400, TEXTO, error};
  }
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  cambiarVelocidad(rpm);
  xSemaphoreGive(candadoEstado);
  return Respuesta{200, TEXTO, "Velocidad: " + String(rpm) + " rpm"};
}

// GET /disparo?vueltas=0.25&sentido=der&retroceso=0.1&modo=12&segundos1=1&segundos2=3
//   -> avanza 0,25 vueltas a la derecha, retrocede 0,1 vueltas (opcional; sin
//      retroceso o con 0 no retrocede), espera DISPARO_ESPERA_MS y enciende
//      los reles segun 'modo': 1, 2, 12 (rele 1 y luego rele 2) o 21 (al
//      reves), el rele 1 durante segundos1 y el rele 2 durante segundos2.
Respuesta ordenDisparo(const char* consulta) {
  float vueltas; long pulsos; bool derecha;
  String error = leerGiro(consulta, vueltas, pulsos, derecha);
  if (error != "") return Respuesta{400, TEXTO, error};
  String textoRetroceso = argumento(consulta, "retroceso");
  textoRetroceso.replace(",", ".");
  textoRetroceso.replace("%2C", ".");
  float retroceso = textoRetroceso.toFloat();
  if (retroceso < 0 || retroceso > VUELTAS_MAX) {
    return Respuesta{400, TEXTO, "Retroceso no válido."};
  }
  long pulsosRetroceso = (long)(retroceso * PULSOS_POR_VUELTA + 0.5f);
  String modo = argumento(consulta, "modo");
  if (modo != "1" && modo != "2" && modo != "12" && modo != "21") {
    return Respuesta{400, TEXTO, "Modo de disparo no válido: 1, 2, 12 o 21."};
  }
  // solo hacen falta los tiempos de los reles que se usan
  float s1 = (modo == "2") ? 1 : segundosDe(consulta, "segundos1");
  float s2 = (modo == "1") ? 1 : segundosDe(consulta, "segundos2");
  if (s1 < 0 || s2 < 0) return Respuesta{400, TEXTO, errorTiempoRele()};

  bool ocupado;
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  ocupado = (pulsosPendientes > 0);
  if (!ocupado) {
    disparoModo = modo;
    disparoMs1 = (unsigned long)(s1 * 1000 + 0.5f);
    disparoMs2 = (unsigned long)(s2 * 1000 + 0.5f);
    disparoPulsosRetroceso = pulsosRetroceso;
    disparoDerecha = derecha;
    faseDisparo = DISPARO_GIRANDO;
    moverMotor(pulsos, derecha);
  }
  xSemaphoreGive(candadoEstado);

  if (ocupado) {
    return Respuesta{409, TEXTO, "El motor está girando. Espera a que termine o pulsa Parar."};
  }
  String mensaje = "Disparo: ";
  mensaje += String(vueltas * 360, 0);
  mensaje += derecha ? "° a la derecha, " : "° a la izquierda, ";
  if (pulsosRetroceso > 0) {
    mensaje += String(retroceso * 360, 0);
    mensaje += "° de vuelta, ";
  }
  mensaje += "espera de ";
  mensaje += String(DISPARO_ESPERA_MS / 1000.0, 1);
  mensaje += " s y ";
  if (modo == "1") mensaje += "relé 1";
  else if (modo == "2") mensaje += "relé 2";
  else if (modo == "12") mensaje += "relé 1 y luego relé 2";
  else mensaje += "relé 2 y luego relé 1";
  return Respuesta{200, TEXTO, mensaje};
}

// GET /automatico?activo=1&modo=12&segundos1=1&segundos2=3
//   -> guarda la secuencia que se lanza sola al detectar pienso: activada o
//      no, modo de los reles (1, 2, 12 o 21) y sus tiempos. Se mantiene al
//      apagar el ESP32.
Respuesta ordenAutomatico(const char* consulta) {
  String activo = argumento(consulta, "activo");
  if (activo != "0" && activo != "1") {
    return Respuesta{400, TEXTO, "activo tiene que ser 0 o 1."};
  }
  String modo = argumento(consulta, "modo");
  if (modo != "1" && modo != "2" && modo != "12" && modo != "21") {
    return Respuesta{400, TEXTO, "Modo no válido: 1, 2, 12 o 21."};
  }
  float s1 = segundosDe(consulta, "segundos1");
  float s2 = segundosDe(consulta, "segundos2");
  if (s1 < 0 || s2 < 0) return Respuesta{400, TEXTO, errorTiempoRele()};

  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  autoActivo = (activo == "1");
  autoModo = modo;
  autoMs1 = (unsigned long)(s1 * 1000 + 0.5f);
  autoMs2 = (unsigned long)(s2 * 1000 + 0.5f);
  xSemaphoreGive(candadoEstado);
  guardarAutomatico();   // la memoria se escribe fuera del candado: tarda unos ms

  String mensaje = "Disparo automático ";
  if (activo == "1") {
    mensaje += "activado: ";
    if (modo == "1") mensaje += "relé 1 " + String(s1, 1) + " s";
    else if (modo == "2") mensaje += "relé 2 " + String(s2, 1) + " s";
    else if (modo == "12") mensaje += "relé 1 " + String(s1, 1) + " s y luego relé 2 " + String(s2, 1) + " s";
    else mensaje += "relé 2 " + String(s2, 1) + " s y luego relé 1 " + String(s1, 1) + " s";
  } else {
    mensaje += "desactivado";
  }
  return Respuesta{200, TEXTO, mensaje};
}

// GET /pienso  -> simula que el sensor detecta pienso (maqueta hasta que haya sensor)
Respuesta ordenPienso(const char* consulta) {
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  bool disparado = piensoDetectado("simulado");
  xSemaphoreGive(candadoEstado);
  return Respuesta{200, TEXTO, disparado
                   ? "Pienso detectado (simulado): secuencia automática lanzada."
                   : "Pienso detectado (simulado), pero el disparo automático está desactivado."};
}

// GET /estado  -> {"moviendo":true,"pulsos_pendientes":120,"sentido":"derecha",
//                  "pulsos_por_vuelta":200,"con_corriente":true,
//                  "rele":false,"rele_ms_restantes":0,"rele2":true,"rele2_ms_restantes":2500,
//                  "serie_siguiente":1,"serie_ms":1500,"disparo":"girando","disparo_modo":"12",
//                  "rpm":25,"auto_activo":true,"auto_modo":"21","pienso_detecciones":3}
// ("rele" y "rele_ms_restantes" son los del rele 1, con los nombres de siempre;
//  serie_siguiente es el rele que se enciende despues en una serie, 0 si no hay;
//  disparo es "girando" (avance), "retrocediendo", "esperando" o "" si no hay
//  ninguno pendiente)
Respuesta ordenEstado(const char* consulta) {
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  long pendientes = pulsosPendientes;
  bool derecha = sentidoDerecha;
  bool conCorriente = driverActivo;
  bool rele = reles[0].encendido;
  unsigned long restante = releRestante(0);
  bool rele2 = reles[1].encendido;
  unsigned long restante2 = releRestante(1);
  int serieSig = serieSiguiente + 1;     // 0 = no hay serie pendiente
  unsigned long msSerie = serieSiguiente >= 0 ? serieMs : 0;
  const char* fase = faseDisparo == DISPARO_GIRANDO ? "girando"
                   : faseDisparo == DISPARO_RETROCEDIENDO ? "retrocediendo"
                   : faseDisparo == DISPARO_ESPERANDO ? "esperando" : "";
  String modo = faseDisparo != DISPARO_NO ? disparoModo : "";
  int rpm = rpmActual;
  bool automatico = autoActivo;
  String modoAuto = autoModo;
  unsigned long detecciones = piensoDetecciones;
  xSemaphoreGive(candadoEstado);

  // Se compone de una vez en un bufer: la pagina lo pide cada segundo y asi
  // no hay que ir alargando un String (y pidiendo memoria) campo a campo.
  bool fotoD15 = digitalRead(FOTO1_PIN) == LOW;
  bool fotoD16 = digitalRead(FOTO2_PIN) == LOW;
  char json[576];
  snprintf(json, sizeof(json),
           "{\"moviendo\":%s,\"pulsos_pendientes\":%ld,\"sentido\":\"%s\","
           "\"pulsos_por_vuelta\":%d,\"con_corriente\":%s,"
           "\"rele\":%s,\"rele_ms_restantes\":%lu,\"rele2\":%s,\"rele2_ms_restantes\":%lu,"
           "\"serie_siguiente\":%d,\"serie_ms\":%lu,\"disparo\":\"%s\",\"disparo_modo\":\"%s\","
           "\"rpm\":%d,\"auto_activo\":%s,\"auto_modo\":\"%s\",\"pienso_detecciones\":%lu,\"foto_d15\":%s,\"foto_d16\":%s}",
           pendientes > 0 ? "true" : "false", pendientes, derecha ? "derecha" : "izquierda",
           PULSOS_POR_VUELTA, conCorriente ? "true" : "false",
           rele ? "true" : "false", restante, rele2 ? "true" : "false", restante2,
           serieSig, msSerie, fase, modo.c_str(),
           rpm, automatico ? "true" : "false", modoAuto.c_str(), detecciones,
           fotoD15 ? "true" : "false", fotoD16 ? "true" : "false");
  return Respuesta{200, JSON, json};
}

// GET /diagnostico  -> {"segundos":95,"memoria_libre":211000,"memoria_minima":209000,
//                       "bloque_mayor":110000,"peticiones":40,"sin_enviar":0,
//                       "envio_max_ms":3,"atencion_max_ms":5,"pasos_medidos":3000,
//                       "paso_desvio_max_us":45,"pasos_irregulares":0,
//                       "bucle_max_us":1150,"modo_wifi":"ap","dispositivos":1,"rssi":0}
// Contadores desde la ultima puesta a cero (la linea [sistema] los pone a
// cero cada DIAGNOSTICO_MS, y esta orden con ?reiniciar=1 despues de leerlos).
// pasos_medidos son los cambios de STEP y paso_desvio_max_us cuanto se ha
// apartado el peor del medio periodo; pasos_irregulares, los que se apartan
// mas del 20%. Con pulsos regulares el motor gira suave.
Respuesta ordenDiagnostico(const char* consulta) {
  const char* modo = modoWifi == MODO_AP ? "ap" : modoWifi == MODO_HOST ? "host"
                   : modoWifi == MODO_BUSCANDO ? "buscando" : "apagado";
  int rssi = (modoWifi == MODO_HOST && WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  char json[480];
  snprintf(json, sizeof(json),
           "{\"segundos\":%lu,\"memoria_libre\":%lu,\"memoria_minima\":%lu,\"bloque_mayor\":%lu,"
           "\"peticiones\":%lu,\"sin_enviar\":%lu,\"envio_max_ms\":%lu,\"atencion_max_ms\":%lu,"
           "\"pasos_medidos\":%lu,\"paso_desvio_max_us\":%lu,\"pasos_irregulares\":%lu,"
           "\"bucle_max_us\":%lu,\"modo_wifi\":\"%s\",\"dispositivos\":%d,\"rssi\":%d}",
           millis() / 1000, (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
           (unsigned long)ESP.getMaxAllocHeap(), webPeticiones, webFallos, webEnvioMaxMs,
           webAtencionMaxMs, pasoCambios, pasoDesvioMaxUs, pasoIrregulares, bucleMaxUs, modo,
           WiFi.softAPgetStationNum(), rssi);
  if (argumento(consulta, "reiniciar") == "1") reiniciarContadores();
  return Respuesta{200, JSON, json};
}

// GET /wifi?modo=ap  -> cambia de modo como el boton BOOT: ap (red propia),
// host (WiFi de casa) o apagado (sin WiFi, para pruebas por el puerto serie).
// Como BOOT, para el motor y apaga los reles. Por la web se pierde la
// conexion, asi que es sobre todo para el puerto serie.
Respuesta ordenWifi(const char* consulta) {
  String modo = argumento(consulta, "modo");
  if (modo == "ap") wifiPedido = MODO_AP;
  else if (modo == "host") wifiPedido = MODO_BUSCANDO;
  else if (modo == "apagado") wifiPedido = MODO_APAGADO;
  else return Respuesta{400, TEXTO, "Modo no válido: ap, host o apagado."};
  return Respuesta{200, TEXTO, "Cambiando a modo " + modo};
}

// Ordenes que se atienden igual por la web y por el puerto serie
typedef Respuesta (*Orden)(const char* consulta);
struct Ruta {
  const char* uri;
  Orden orden;
};
const Ruta RUTAS[] = {
  { "/mover",       ordenMover },
  { "/parar",       ordenParar },
  { "/estado",      ordenEstado },
  { "/rele",        ordenRele },
  { "/serie",       ordenSerie },
  { "/disparo",     ordenDisparo },
  { "/velocidad",   ordenVelocidad },
  { "/automatico",  ordenAutomatico },
  { "/pienso",      ordenPienso },
  { "/diagnostico", ordenDiagnostico },
  { "/wifi",        ordenWifi },
};
const int NUM_RUTAS = sizeof(RUTAS) / sizeof(RUTAS[0]);

// Peticion de la web a una de las RUTAS: saca la consulta, ejecuta la orden
// (user_ctx) y envia la respuesta.
esp_err_t atenderOrden(httpd_req_t* req) {
  unsigned long inicio = millis();
  char consulta[160];
  if (httpd_req_get_url_query_str(req, consulta, sizeof(consulta)) != ESP_OK) consulta[0] = '\0';
  Respuesta r = ((Orden)req->user_ctx)(consulta);
  esp_err_t resultado = responder(req, r.codigo, r.tipo, r.cuerpo);
  unsigned long ms = millis() - inicio;
  if (ms > webAtencionMaxMs) webAtencionMaxMs = ms;
  return resultado;
}

// Se llama en loop(): lee las ordenes que llegan por el puerto serie, una por
// linea y con la misma forma que en la web (/mover?vueltas=0.25&sentido=der),
// y contesta con una linea "> codigo texto". Asi un programa del PC controla
// el motor por el cable USB, sin WiFi.
void atenderSerie() {
  static char linea[200];
  static size_t largo = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (largo < sizeof(linea) - 1) linea[largo++] = c;
      continue;
    }
    linea[largo] = '\0';
    largo = 0;
    if (linea[0] == '\0') continue;
    char* interrogacion = strchr(linea, '?');
    const char* consulta = "";
    if (interrogacion) {
      *interrogacion = '\0';
      consulta = interrogacion + 1;
    }
    Respuesta r = { 404, TEXTO, "Orden no encontrada. Ejemplos: /estado, /parar, /mover?vueltas=0.25&sentido=der" };
    for (int i = 0; i < NUM_RUTAS; i++) {
      if (strcmp(linea, RUTAS[i].uri) == 0) {
        r = RUTAS[i].orden(consulta);
        break;
      }
    }
    Serial.printf("> %d %s\n", r.codigo, r.cuerpo.c_str());
  }
}

// Cualquier otra direccion (por ejemplo, el icono /favicon.ico)
esp_err_t paginaNoEncontrada(httpd_req_t* req, httpd_err_code_t error) {
  responder(req, 404, "text/plain; charset=utf-8", "No encontrado");
  return ESP_OK;   // ESP_OK mantiene abierta la conexion para la siguiente peticion
}

// Arranca el servidor web y registra las direcciones. Se arranca una vez y
// sigue funcionando al cambiar entre modo AP y host: escucha en todas las
// redes a la vez.
void arrancarServidorWeb() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_open_sockets = WEB_MAX_CONEXIONES;
  config.lru_purge_enable = true;   // si se llena, cierra la conexion menos usada
  config.stack_size = 8192;         // margen para construir la pagina y los textos
  config.max_uri_handlers = 16;     // por defecto solo admite 8 direcciones

  // El servidor atiende las peticiones de una en una. Si un movil deja una
  // conexion a medias (pantalla apagada, cambio a datos moviles...), el
  // servidor espera en ella hasta recv/send_wait_timeout antes de seguir, y
  // mientras tanto no atiende a nadie. Con los 5 s por defecto, unas pocas
  // conexiones asi frenaban la web hasta 20 s. Se baja a WEB_ESPERA_S, y el
  // keep-alive de TCP cierra las conexiones cuyo movil ya no contesta.
  config.recv_wait_timeout = WEB_ESPERA_S;
  config.send_wait_timeout = WEB_ESPERA_S;
  config.keep_alive_enable = true;
  config.keep_alive_idle = 5;       // segundos sin trafico antes de comprobar
  config.keep_alive_interval = 2;   // segundos entre comprobaciones
  config.keep_alive_count = 3;      // comprobaciones sin respuesta para cerrarla

  if (httpd_start(&servidor, &config) != ESP_OK) {
    aviso("[web] ERROR: no se pudo arrancar el servidor web");
    return;
  }
  httpd_uri_t pagina = { .uri = "/", .method = HTTP_GET, .handler = paginaPrincipal, .user_ctx = NULL };
  httpd_register_uri_handler(servidor, &pagina);
  for (int i = 0; i < NUM_RUTAS; i++) {
    httpd_uri_t ruta = { .uri = RUTAS[i].uri, .method = HTTP_GET, .handler = atenderOrden,
                         .user_ctx = (void*)RUTAS[i].orden };
    httpd_register_uri_handler(servidor, &ruta);
  }
  httpd_register_err_handler(servidor, HTTPD_404_NOT_FOUND, paginaNoEncontrada);
  aviso("[web] servidor web en marcha (puerto 80)");
}


// ---------------------------------------------------------------------
//  WIFI
// ---------------------------------------------------------------------

// Redes WiFi que se prueban en orden en modo host (claves en credenciales.h).
const char* REDES_SSID[]  = { WIFI_ROUTER_SSID,     WIFI_ROUTER2_SSID };
const char* REDES_CLAVE[] = { WIFI_ROUTER_PASSWORD, WIFI_ROUTER2_PASSWORD };
const int   NUM_REDES = sizeof(REDES_SSID) / sizeof(REDES_SSID[0]);
int redActual = -1;                 // red que se esta probando
unsigned long inicioIntento = 0;    // instante (millis) en que empezo a probarla

// Pone el LED segun el modo. Se llama en cada pasada de loop() y no bloquea.
//   modo AP: parpadeo | buscando o conexion perdida: el doble de rapido | conectado: fijo
void actualizarLed() {
  static unsigned long ultimoCambio = 0;
  static bool encendido = false;

  if (modoWifi == MODO_HOST && WiFi.status() == WL_CONNECTED) {
    if (LED_PIN >= 0) digitalWrite(LED_PIN, LED_ENCENDIDO);
    encendido = true;
    return;
  }
  if (modoWifi == MODO_APAGADO) {
    if (LED_PIN >= 0) digitalWrite(LED_PIN, !LED_ENCENDIDO);
    encendido = false;
    return;
  }
  unsigned long medioPeriodo = (modoWifi == MODO_AP) ? LED_AP_MS : LED_AP_MS / 2;
  if (millis() - ultimoCambio >= medioPeriodo) {
    ultimoCambio = millis();
    encendido = !encendido;
    if (LED_PIN >= 0) digitalWrite(LED_PIN, encendido ? LED_ENCENDIDO : !LED_ENCENDIDO);
  }
}

// Publica el nombre motor.local en la red actual.
void iniciarMdns() {
  MDNS.end();
  if (MDNS.begin(NOMBRE_RED)) {
    MDNS.addService("http", "tcp", 80);
  }
}

// Modo AP: crea la red propia ESP32_MOTOR.
void entrarModoAP() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(WIFI_SSID, WIFI_PASSWORD, WIFI_CANAL)) {
    aviso("[wifi] ERROR: no se pudo crear la red (la clave debe tener al menos 8 caracteres)");
  }
  WiFi.setTxPower(WIFI_POTENCIA);
  modoWifi = MODO_AP;
  iniciarMdns();

  aviso(String("[wifi] modo AP: red ") + WIFI_SSID + " (clave " + WIFI_PASSWORD +
        "), web en http://" + WiFi.softAPIP().toString());
  aviso("[wifi] pulsa BOOT para pasar a modo host");
}

// Empieza a probar la siguiente red de la lista. Si no quedan, vuelve a AP.
void probarSiguienteRed() {
  do {
    redActual++;
  } while (redActual < NUM_REDES && REDES_SSID[redActual][0] == '\0');  // salta redes sin configurar

  if (redActual >= NUM_REDES) {
    aviso("[wifi] no se pudo conectar a ninguna red: vuelvo al modo AP");
    entrarModoAP();
    return;
  }
  aviso(String("[wifi] probando ") + REDES_SSID[redActual] + " (hasta " +
        String(TIEMPO_CONEXION_MS / 1000) + " s)");
  WiFi.disconnect();
  WiFi.begin(REDES_SSID[redActual], REDES_CLAVE[redActual]);
  inicioIntento = millis();
}

// Modo host: apaga la red propia y empieza a buscar las redes de la lista.
void empezarBusqueda() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  WiFi.setHostname(NOMBRE_RED);
  WiFi.mode(WIFI_STA);
  // Sin ahorro de energia: con el, la WiFi duerme entre balizas del router y
  // cada peticion de la web puede esperar cientos de ms a que despierte.
  WiFi.setSleep(false);
  modoWifi = MODO_BUSCANDO;
  redActual = -1;
  aviso("[wifi] modo host: buscando red");
  probarSiguienteRed();
}

// Se llama en loop(): sigue la busqueda sin bloquear. Pasa a MODO_HOST al
// conectar, o a la siguiente red cuando se agota TIEMPO_CONEXION_MS.
void actualizarWifi() {
  if (modoWifi != MODO_BUSCANDO) return;

  if (WiFi.status() == WL_CONNECTED) {
    modoWifi = MODO_HOST;
    iniciarMdns();
    aviso(String("[wifi] conectado a ") + REDES_SSID[redActual] + ", web en http://" +
          WiFi.localIP().toString() + " o http://" + NOMBRE_RED + ".local");
  } else if (millis() - inicioIntento >= TIEMPO_CONEXION_MS) {
    aviso(String("[wifi] ") + REDES_SSID[redActual] + " no disponible");
    probarSiguienteRed();
  }
}

// Sin WiFi (solo para pruebas, con /wifi?modo=apagado).
void apagarWifi() {
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  modoWifi = MODO_APAGADO;
  aviso("[wifi] WiFi apagada (solo puerto serie)");
}

// Cambia al modo 'destino': MODO_AP, MODO_BUSCANDO (modo host) o
// MODO_APAGADO. Antes para el motor y apaga los reles, porque se pierde la
// conexion con la web.
void cambiarModoWifi(ModoWifi destino, const char* motivo) {
  aviso(String(motivo) + ": paro motor, disparo y reles y cambio de modo");
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  cancelarDisparo();
  pararMotor();
  cancelarSerie();
  for (int i = 0; i < NUM_RELES; i++) {
    if (reles[i].encendido) apagarRele(i);
  }
  xSemaphoreGive(candadoEstado);
  if (destino == MODO_BUSCANDO) empezarBusqueda();
  else if (destino == MODO_APAGADO) apagarWifi();
  else entrarModoAP();
}

// Se llama en loop(): cada DIAGNOSTICO_MS escribe la linea [sistema] y pone a
// cero los contadores de la web.
void diagnostico() {
  static unsigned long ultimo = 0;
  if (!DEPURACION || millis() - ultimo < DIAGNOSTICO_MS) return;
  ultimo = millis();
  unsigned long peticiones = webPeticiones, fallos = webFallos, envioMax = webEnvioMaxMs;
  String linea = "[sistema] ";
  linea += DIAGNOSTICO_MS / 1000;
  linea += " s: ";
  linea += peticiones;
  linea += " peticiones, ";
  linea += fallos;
  linea += " sin enviar, envio mas lento ";
  linea += envioMax;
  linea += " ms, peticion mas lenta ";
  linea += webAtencionMaxMs;
  linea += " ms | pulsos: desvio max ";
  linea += pasoDesvioMaxUs;
  linea += " us, ";
  linea += pasoIrregulares;
  linea += " irregulares | bucle max ";
  linea += bucleMaxUs;
  linea += " us | memoria libre ";
  linea += ESP.getFreeHeap();
  linea += " B, minima ";
  linea += ESP.getMinFreeHeap();
  linea += " B, bloque mayor ";
  linea += ESP.getMaxAllocHeap();
  linea += " B | en la red propia: ";
  linea += WiFi.softAPgetStationNum();
  aviso(linea);
  reiniciarContadores();
}

// Se llama en loop(): detecta la pulsacion corta de BOOT, con antirrebote.
// No hace caso hasta pasados ARRANQUE_GRACIA_MS y haber visto el boton suelto.
void leerBoot() {
  static bool habilitado = false;
  static bool pulsadoAntes = false;
  static unsigned long ultimoCambio = 0;

  bool pulsado = (digitalRead(BOTON_MODO_PIN) == LOW);
  if (!habilitado) {
    if (millis() >= ARRANQUE_GRACIA_MS && !pulsado) habilitado = true;
    return;
  }
  if (pulsado != pulsadoAntes && millis() - ultimoCambio >= ANTIRREBOTE_MS) {
    ultimoCambio = millis();
    pulsadoAntes = pulsado;
    // de AP a host; de host, buscando o apagado, a AP
    if (pulsado) cambiarModoWifi(modoWifi == MODO_AP ? MODO_BUSCANDO : MODO_AP, "[boton] BOOT pulsado");
  }
}


// ---------------------------------------------------------------------
//  SETUP Y LOOP
// ---------------------------------------------------------------------

void setup() {
  // Bufer de envio del Monitor Serie: sin el, cada linea deja esperando a
  // quien la escribe hasta que sale por el cable (a 115200 baudios, unos 9 ms
  // cada 100 caracteres; la linea [sistema] paraba loop() 24 ms).
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  delay(500);

  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN, DIR_DERECHA);

  // Driver: sin corriente al arrancar si se apaga en reposo; si no, siempre activo
  pinMode(ENABLE_PIN, OUTPUT);
  if (DESACTIVAR_EN_REPOSO) {
    digitalWrite(ENABLE_PIN, !ENABLE_ACTIVO);
    driverActivo = false;
  } else {
    digitalWrite(ENABLE_PIN, ENABLE_ACTIVO);
    driverActivo = true;
  }

  // Reles apagados desde el primer momento (se fija el nivel antes de hacerlos salida)
  for (int i = 0; i < NUM_RELES; i++) {
    digitalWrite(reles[i].pin, !RELE_ACTIVO);
    pinMode(reles[i].pin, OUTPUT);
    digitalWrite(reles[i].pin, !RELE_ACTIVO);
  }

  if (LED_PIN >= 0) { pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, !LED_ENCENDIDO); }
  pinMode(FOTO1_PIN, INPUT_PULLUP);
  pinMode(FOTO2_PIN, INPUT_PULLUP);

  // Candado entre la web y loop() (ver candadoEstado)
  candadoEstado = xSemaphoreCreateMutex();

  // Velocidad inicial y secuencia automatica guardada
  cambiarVelocidad(RPM_INICIAL);
  ajustes.begin("motor", false);
  leerAutomatico();

  // Temporizador hardware que da los pulsos, contando en microsegundos. Se
  // crea aqui, en el nucleo 1, para que su interrupcion corra en ese nucleo.
  // timerBegin lo deja en marcha: se para hasta que moverMotor lo arranque.
  temporizadorPasos = timerBegin(1000000);
  timerStop(temporizadorPasos);
  timerAttachInterrupt(temporizadorPasos, &darPaso);

  Serial.println();
  Serial.println("Control motor paso a paso");

  // Boton BOOT (en la placa ya tiene resistencia a 3,3 V; se anade la interna)
  pinMode(BOTON_MODO_PIN, INPUT_PULLUP);

  // Avisos de WiFi por el Monitor Serie (quien se conecta, router...)
  WiFi.onEvent(eventoWifi);

  // Siempre se arranca en modo AP; BOOT pasa a modo host
  entrarModoAP();

  Serial.print("Pulsos por vuelta: ");
  Serial.println(PULSOS_POR_VUELTA);

  // Servidor web (atiende en su propia tarea; loop() no tiene que llamarlo)
  prepararPagina();
  arrancarServidorWeb();
}

void loop() {
  // Diagnostico: tiempo desde la pasada anterior (incluye el delay(1) del final)
  static unsigned long pasadaAnterior = 0;
  unsigned long ahora = micros();
  if (pasadaAnterior != 0 && ahora - pasadaAnterior > bucleMaxUs) bucleMaxUs = ahora - pasadaAnterior;
  pasadaAnterior = ahora;

  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  comprobarFinMovimiento();   // para el temporizador al acabar los pulsos
  comprobarDisparo();         // tras el giro del disparo, espera y enciende los reles
  comprobarReles();           // apaga cada rele cuando acaba su tiempo (y sigue las series)
  xSemaphoreGive(candadoEstado);

  atenderSerie();             // ordenes por el puerto serie (las mismas que la web)
  if (wifiPedido >= 0) {      // cambio de modo pedido con /wifi
    ModoWifi destino = (ModoWifi)wifiPedido;
    wifiPedido = -1;
    cambiarModoWifi(destino, "[wifi] orden /wifi");
  }
  leerBoot();                 // pulsacion de BOOT: cambia entre modo AP y host
  actualizarWifi();           // sigue la busqueda de red en modo host
  actualizarLed();            // parpadeo en AP, doble de rapido buscando, fijo conectado
  diagnostico();              // cada DIAGNOSTICO_MS, la linea [sistema] por el Monitor Serie
  delay(1);                   // cede el procesador a las demas tareas (web, WiFi)
}
