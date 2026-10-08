/*
  CONTROL DE MOTOR PASO A PASO CON ESP32 POR WIFI
  ===============================================

  El ESP32 sirve una pagina web sencilla para mover un motor paso a paso con
  un driver de entradas STEP y DIR (A4988, DRV8825, TMC2208, TB6600, DM542...).

  MODOS WIFI
    - Al arrancar crea su propia red ESP32_MOTOR (modo AP), web en
      http://192.168.4.1. LED parpadeando.
    - Una pulsacion corta del boton BOOT (pasados ARRANQUE_GRACIA_MS desde el
      arranque) para el motor y el rele y pasa a modo host: busca la WiFi de
      casa y despues la segunda red, TIEMPO_CONEXION_MS cada una. LED
      parpadeando el doble de rapido.
    - Si conecta: LED fijo, web en http://motor.local o en la IP que sale por
      el Monitor Serie. Si no conecta a ninguna, vuelve al modo AP.
    - Otra pulsacion de BOOT para el motor y el rele y vuelve al modo AP.

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

    ESP32               Modulo de rele
    -----               --------------
    GPIO 27  (RELE_PIN) IN
    GND                 GND
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
    Por eso los pulsos los da un temporizador del ESP32 (esp_timer) que salta
    cada PULSO_US microsegundos, pase lo que pase con la web.
*/

#include <WiFi.h>
#include <ESPmDNS.h>          // nombre motor.local (viene con el core ESP32)
#include "esp_timer.h"        // temporizador de los pulsos (viene con el core ESP32)
#include "esp_http_server.h"  // servidor web del sistema del ESP32 (viene con el core)
#include "lwip/sockets.h"     // para saber la IP de quien hace cada peticion

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

// Rele temporizado: el boton de la web lo enciende durante el tiempo
// indicado y luego se apaga solo.
#define RELE_PIN            27      // GPIO conectado a la entrada IN del modulo de rele (D27)
                                    // No usar GPIO 14 ni 15: dan pulsos durante el arranque
                                    // del ESP32 y el rele chasquearia al encender
#define RELE_ACTIVO         HIGH    // nivel que enciende el rele. Muchos modulos con
                                    // optoacoplador se activan con LOW: si el rele se
                                    // enciende al arrancar y se apaga al pulsar, pon LOW
#define RELE_TIEMPO_S       1       // tiempo por defecto en la web, en segundos
#define RELE_MAX_S          600     // tiempo maximo por orden, en segundos

// Pulsos que necesita el driver para una vuelta completa del motor.
// Motor de 200 pasos sin microstepping = 200.
// Con microstepping se multiplica: 1/2 = 400, 1/8 = 1600, 1/16 = 3200...
#define PULSOS_POR_VUELTA   200

// Tiempo en microsegundos que STEP esta en ALTO y despues en BAJO.
// Un paso completo dura 2 x PULSO_US. Con 1000 us: 500 pasos por segundo,
// es decir 2,5 vueltas por segundo con 200 pulsos por vuelta.
// Menos tiempo = mas velocidad. Si el motor vibra sin girar o pierde pasos,
// aumenta este valor.
#define PULSO_US            1000

// LED de estado de la WiFi (el LED azul de la placa suele estar en GPIO 2):
//   parpadeo                  = modo AP, red propia ESP32_MOTOR
//   parpadeo el doble de rapido = modo host buscando red (o conexion perdida)
//   fijo                      = modo host conectado
#define LED_PIN             2
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

// Depuracion por el Monitor Serie (115200 baudios): dispositivos que se
// conectan a la red propia, conexion con el router y cada peticion a la web,
// con la hora en segundos desde el arranque. DEPURAR_ESTADO muestra tambien
// las consultas /estado que la pagina hace cada segundo (por defecto no, para
// que no tapen lo demas).
#define DEPURACION          true
#define DEPURAR_ESTADO      false

// =====================================================================

// Los GPIO 34 a 39 del ESP32 son solo de entrada: no pueden mover el driver.
#if (STEP_PIN >= 34 && STEP_PIN <= 39) || (DIR_PIN >= 34 && DIR_PIN <= 39) || (ENABLE_PIN >= 34 && ENABLE_PIN <= 39) || (RELE_PIN >= 34 && RELE_PIN <= 39)
#error "STEP_PIN, DIR_PIN, ENABLE_PIN y RELE_PIN no pueden ser los GPIO 34 a 39: en el ESP32 son solo de entrada. Usa por ejemplo 33, 32, 25 y 27."
#endif

// El temporizador del ESP32 no admite periodos menores de 50 us.
#if PULSO_US < 50
#error "PULSO_US debe ser de 50 microsegundos o mas."
#endif

httpd_handle_t servidor = NULL;

// La web se atiende en la tarea del servidor y el resto (fin de movimiento,
// rele, boton, WiFi) en loop(). Este candado evita que las dos toquen el
// estado del motor y del rele a la vez.
SemaphoreHandle_t candadoEstado;

// Estado del movimiento. moverMotor() lo prepara y el temporizador da los
// pulsos. 'volatile' porque el temporizador corre en otra tarea del ESP32.
volatile long pulsosPendientes = 0;   // pulsos que faltan por dar
volatile bool stepEnAlto = false;     // estado actual de la salida STEP
bool sentidoDerecha = true;           // sentido del movimiento en curso
bool temporizadorEnMarcha = false;
bool driverActivo = false;            // true = bobinas con corriente
unsigned long finMovimiento = 0;      // instante (millis) en que paro el motor

// Estado del rele
bool releEncendido = false;
unsigned long releInicio = 0;         // instante (millis) en que se encendio
unsigned long releDuracionMs = 0;     // cuanto tiempo tiene que estar encendido

esp_timer_handle_t temporizadorPasos;
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

// Valor de un parametro de la direccion (?vueltas=1.5&...). Vacio si no esta.
String argumento(httpd_req_t* req, const char* nombre) {
  char consulta[128];
  char valor[32];
  if (httpd_req_get_url_query_str(req, consulta, sizeof(consulta)) != ESP_OK) return "";
  if (httpd_query_key_value(consulta, nombre, valor, sizeof(valor)) != ESP_OK) return "";
  return String(valor);
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
  esp_err_t resultado = httpd_resp_send(req, cuerpo.c_str(), cuerpo.length());

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

// Lo llama el temporizador cada PULSO_US microsegundos. Alterna STEP: un
// pulso es ALTO durante PULSO_US y BAJO durante PULSO_US, y cuenta como dado
// al bajar STEP.
void darPaso(void* arg) {
  portENTER_CRITICAL(&candadoMotor);
  if (pulsosPendientes > 0) {
    if (!stepEnAlto) {
      digitalWrite(STEP_PIN, HIGH);
      stepEnAlto = true;
    } else {
      digitalWrite(STEP_PIN, LOW);
      stepEnAlto = false;
      pulsosPendientes = pulsosPendientes - 1;
    }
  }
  portEXIT_CRITICAL(&candadoMotor);
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
  if (temporizadorEnMarcha) esp_timer_stop(temporizadorPasos);
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
  portEXIT_CRITICAL(&candadoMotor);
  esp_timer_start_periodic(temporizadorPasos, PULSO_US);
  temporizadorEnMarcha = true;
}

// Detiene el motor inmediatamente.
void pararMotor() {
  if (temporizadorEnMarcha) esp_timer_stop(temporizadorPasos);
  temporizadorEnMarcha = false;
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
    esp_timer_stop(temporizadorPasos);
    temporizadorEnMarcha = false;
    finMovimiento = millis();
    aviso("[motor] movimiento terminado");
  }
  if (DESACTIVAR_EN_REPOSO && driverActivo && !temporizadorEnMarcha &&
      millis() - finMovimiento >= REPOSO_MS) {
    desactivarDriver();
  }
}


// ---------------------------------------------------------------------
//  RELE
// ---------------------------------------------------------------------

// Enciende el rele durante 'ms' milisegundos. Si ya estaba encendido,
// vuelve a empezar a contar con el tiempo nuevo.
void encenderRele(unsigned long ms) {
  digitalWrite(RELE_PIN, RELE_ACTIVO);
  releEncendido = true;
  releInicio = millis();
  releDuracionMs = ms;
}

void apagarRele() {
  digitalWrite(RELE_PIN, !RELE_ACTIVO);
  releEncendido = false;
}

// Se llama en loop(): apaga el rele cuando ha pasado su tiempo. No bloquea.
void comprobarRele() {
  if (releEncendido && millis() - releInicio >= releDuracionMs) {
    apagarRele();
    aviso("[rele] apagado");
  }
}


// ---------------------------------------------------------------------
//  PAGINA WEB
// ---------------------------------------------------------------------

// %PPV% se sustituye por PULSOS_POR_VUELTA al enviar la pagina.
const char PAGINA[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Control motor paso a paso</title>
<style>
  body { font-family: sans-serif; max-width: 420px; margin: 0 auto; padding: 16px;
         background: #f2f2f2; color: #222; }
  h1 { font-size: 1.4em; text-align: center; }
  .caja { background: #fff; border-radius: 8px; padding: 16px; margin-bottom: 12px; }
  label { display: block; margin-bottom: 6px; }
  input { width: 100%; font-size: 1.3em; padding: 8px; box-sizing: border-box; }
  .fila { display: flex; gap: 10px; }
  button { flex: 1; width: 100%; font-size: 1.05em; padding: 14px 6px; margin-top: 10px;
           border: 0; border-radius: 6px; background: #1565c0; color: #fff; }
  button:active { background: #0d47a1; }
  button.parar { background: #c62828; }
  button.rele { background: #2e7d32; }
  #estado { font-weight: bold; }
</style>
</head>
<body>
  <h1>Control motor paso a paso</h1>

  <div class="caja">Pulsos por vuelta: <b>%PPV%</b></div>

  <div class="caja">
    <label for="vueltas">Número de vueltas</label>
    <input id="vueltas" type="number" min="0" step="any" value="1" inputmode="decimal">
    <div class="fila">
      <button onclick="girar('izq')">Girar izquierda</button>
      <button onclick="girar('der')">Girar derecha</button>
    </div>
    <div class="fila">
      <button onclick="girar('izq', 1)">1 vuelta izquierda</button>
      <button onclick="girar('der', 1)">1 vuelta derecha</button>
    </div>
    <button class="parar" onclick="parar(5)">Parar</button>
  </div>

  <div class="caja">
    <label for="segundos">Tiempo del relé (segundos)</label>
    <input id="segundos" type="number" min="0" step="any" value="%RELE_S%" inputmode="decimal">
    <button class="rele" onclick="activarRele()">Activar relé</button>
  </div>

  <div class="caja">
    Motor: <span id="estado">-</span><br>
    Relé: <span id="rele">-</span><br>
    <span id="mensaje"></span>
  </div>

<script>
  // Envia una orden al ESP32 y muestra su respuesta
  function orden(url) {
    fetch(url)
      .then(r => r.text())
      .then(t => { document.getElementById('mensaje').textContent = t; })
      .catch(() => { document.getElementById('mensaje').textContent = 'Sin conexión con el ESP32'; });
  }

  // Sin 'vueltas' usa el valor del campo; con 'vueltas' = 1 es el boton de 1 vuelta
  function girar(sentido, vueltas) {
    if (vueltas === undefined) {
      vueltas = document.getElementById('vueltas').value.replace(',', '.');
    }
    orden('/mover?vueltas=' + encodeURIComponent(vueltas) + '&sentido=' + sentido);
  }

  // Orden de parar. Si no llega al ESP32, se reintenta hasta 'intentos' veces.
  function parar(intentos) {
    fetch('/parar')
      .then(r => r.text())
      .then(t => { document.getElementById('mensaje').textContent = t; })
      .catch(() => {
        if (intentos > 1) setTimeout(() => parar(intentos - 1), 200);
        else document.getElementById('mensaje').textContent = 'No se pudo enviar Parar';
      });
  }

  // Enciende el rele durante los segundos del campo
  function activarRele() {
    var s = document.getElementById('segundos').value.replace(',', '.');
    orden('/rele?segundos=' + encodeURIComponent(s));
  }

  // Consulta el estado del motor cada segundo
  function leerEstado() {
    fetch('/estado')
      .then(r => r.json())
      .then(e => {
        document.getElementById('estado').textContent = e.moviendo
          ? 'girando a la ' + e.sentido + ', quedan ' + e.pulsos_pendientes + ' pulsos'
          : (e.con_corriente ? 'parado, con corriente' : 'parado, sin corriente');
        document.getElementById('rele').textContent = e.rele
          ? 'encendido, quedan ' + (e.rele_ms_restantes / 1000).toFixed(1) + ' s'
          : 'apagado';
      })
      .catch(() => { document.getElementById('estado').textContent = 'sin conexión'; });
  }
  setInterval(leerEstado, 1000);
  leerEstado();
</script>
</body>
</html>
)rawliteral";


// ---------------------------------------------------------------------
//  RESPUESTAS HTTP
// ---------------------------------------------------------------------

// Cada manejador corre en la tarea del servidor web. Toma candadoEstado solo
// mientras lee o cambia el estado del motor y del rele, y lo suelta ANTES de
// enviar la respuesta: asi un envio lento a un movil con mala cobertura nunca
// frena el bucle principal ni el motor.

// GET /  -> pagina de control
esp_err_t paginaPrincipal(httpd_req_t* req) {
  String html = PAGINA;
  html.replace("%PPV%", String(PULSOS_POR_VUELTA));
  html.replace("%RELE_S%", String(RELE_TIEMPO_S));
  return responder(req, 200, "text/html; charset=utf-8", html);
}

// GET /mover?vueltas=1.5&sentido=der  (sentido: der o izq)
esp_err_t ordenMover(httpd_req_t* req) {
  String textoVueltas = argumento(req, "vueltas");
  textoVueltas.replace(",", ".");          // admite 0,5 ademas de 0.5
  textoVueltas.replace("%2C", ".");        // la coma puede llegar codificada
  float vueltas = textoVueltas.toFloat();
  String sentido = argumento(req, "sentido");

  if (vueltas <= 0 || vueltas > VUELTAS_MAX) {
    String error = "Número de vueltas no válido (entre 0 y ";
    error += VUELTAS_MAX;
    error += ").";
    return responder(req, 400, "text/plain; charset=utf-8", error);
  }
  if (sentido != "der" && sentido != "izq") {
    return responder(req, 400, "text/plain; charset=utf-8", "Sentido no válido.");
  }

  // pulsos = vueltas * PULSOS_POR_VUELTA, redondeado al pulso mas cercano
  long pulsos = (long)(vueltas * PULSOS_POR_VUELTA + 0.5f);
  if (pulsos < 1) {
    return responder(req, 400, "text/plain; charset=utf-8", "Son menos de un pulso.");
  }

  bool derecha = (sentido == "der");
  bool ocupado;
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  ocupado = (pulsosPendientes > 0);
  if (!ocupado) moverMotor(pulsos, derecha);
  xSemaphoreGive(candadoEstado);

  if (ocupado) {
    return responder(req, 409, "text/plain; charset=utf-8",
                     "El motor está girando. Espera a que termine o pulsa Parar.");
  }
  String mensaje = "Girando ";
  mensaje += String(vueltas, 2);
  mensaje += derecha ? " vueltas a la derecha (" : " vueltas a la izquierda (";
  mensaje += pulsos;
  mensaje += " pulsos)";
  return responder(req, 200, "text/plain; charset=utf-8", mensaje);
}

// GET /parar
esp_err_t ordenParar(httpd_req_t* req) {
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  pararMotor();
  xSemaphoreGive(candadoEstado);
  return responder(req, 200, "text/plain; charset=utf-8", "Motor parado.");
}

// GET /rele?segundos=1.5  -> enciende el rele 1,5 s y luego se apaga solo
esp_err_t ordenRele(httpd_req_t* req) {
  String texto = argumento(req, "segundos");
  texto.replace(",", ".");
  texto.replace("%2C", ".");
  float segundos = texto.toFloat();
  if (segundos <= 0 || segundos > RELE_MAX_S) {
    String error = "Tiempo del relé no válido (entre 0 y ";
    error += RELE_MAX_S;
    error += " segundos).";
    return responder(req, 400, "text/plain; charset=utf-8", error);
  }
  unsigned long ms = (unsigned long)(segundos * 1000 + 0.5f);
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  encenderRele(ms);
  xSemaphoreGive(candadoEstado);

  String mensaje = "Relé encendido ";
  mensaje += String(segundos, 1);
  mensaje += " s";
  return responder(req, 200, "text/plain; charset=utf-8", mensaje);
}

// GET /estado  -> {"moviendo":true,"pulsos_pendientes":120,"sentido":"derecha",
//                  "pulsos_por_vuelta":200,"con_corriente":true,"rele":false,"rele_ms_restantes":0}
esp_err_t ordenEstado(httpd_req_t* req) {
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  long pendientes = pulsosPendientes;
  bool derecha = sentidoDerecha;
  bool conCorriente = driverActivo;
  bool rele = releEncendido;
  unsigned long transcurrido = millis() - releInicio;
  unsigned long restante = (rele && transcurrido < releDuracionMs) ? releDuracionMs - transcurrido : 0;
  xSemaphoreGive(candadoEstado);

  String json = "{\"moviendo\":";
  json += (pendientes > 0) ? "true" : "false";
  json += ",\"pulsos_pendientes\":";
  json += pendientes;
  json += ",\"sentido\":\"";
  json += derecha ? "derecha" : "izquierda";
  json += "\",\"pulsos_por_vuelta\":";
  json += PULSOS_POR_VUELTA;
  json += ",\"con_corriente\":";
  json += conCorriente ? "true" : "false";
  json += ",\"rele\":";
  json += rele ? "true" : "false";
  json += ",\"rele_ms_restantes\":";
  json += restante;
  json += "}";
  return responder(req, 200, "application/json", json);
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

  if (httpd_start(&servidor, &config) != ESP_OK) {
    aviso("[web] ERROR: no se pudo arrancar el servidor web");
    return;
  }
  httpd_uri_t rutas[] = {
    { .uri = "/",       .method = HTTP_GET, .handler = paginaPrincipal, .user_ctx = NULL },
    { .uri = "/mover",  .method = HTTP_GET, .handler = ordenMover,      .user_ctx = NULL },
    { .uri = "/parar",  .method = HTTP_GET, .handler = ordenParar,      .user_ctx = NULL },
    { .uri = "/estado", .method = HTTP_GET, .handler = ordenEstado,     .user_ctx = NULL },
    { .uri = "/rele",   .method = HTTP_GET, .handler = ordenRele,       .user_ctx = NULL },
  };
  for (auto& ruta : rutas) httpd_register_uri_handler(servidor, &ruta);
  httpd_register_err_handler(servidor, HTTPD_404_NOT_FOUND, paginaNoEncontrada);
  aviso("[web] servidor web en marcha (puerto 80)");
}


// ---------------------------------------------------------------------
//  WIFI
// ---------------------------------------------------------------------

// Modo de la WiFi
enum ModoWifi { MODO_AP, MODO_BUSCANDO, MODO_HOST };
ModoWifi modoWifi = MODO_AP;

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
    digitalWrite(LED_PIN, LED_ENCENDIDO);
    encendido = true;
    return;
  }
  unsigned long medioPeriodo = (modoWifi == MODO_AP) ? LED_AP_MS : LED_AP_MS / 2;
  if (millis() - ultimoCambio >= medioPeriodo) {
    ultimoCambio = millis();
    encendido = !encendido;
    digitalWrite(LED_PIN, encendido ? LED_ENCENDIDO : !LED_ENCENDIDO);
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

// Cambia de modo al pulsar BOOT: de AP a host, o de host (o buscando) a AP.
// Antes para el motor y apaga el rele, porque se pierde la conexion con la web.
void cambiarModoWifi() {
  aviso("[boton] BOOT pulsado: paro motor y rele y cambio de modo");
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  pararMotor();
  if (releEncendido) apagarRele();
  xSemaphoreGive(candadoEstado);
  if (modoWifi == MODO_AP) {
    empezarBusqueda();
  } else {
    entrarModoAP();
  }
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
    if (pulsado) cambiarModoWifi();
  }
}


// ---------------------------------------------------------------------
//  SETUP Y LOOP
// ---------------------------------------------------------------------

void setup() {
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

  // Rele apagado desde el primer momento (se fija el nivel antes de hacerlo salida)
  digitalWrite(RELE_PIN, !RELE_ACTIVO);
  pinMode(RELE_PIN, OUTPUT);
  digitalWrite(RELE_PIN, !RELE_ACTIVO);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, !LED_ENCENDIDO);

  // Candado entre la web y loop() (ver candadoEstado)
  candadoEstado = xSemaphoreCreateMutex();

  // Temporizador que da los pulsos (se arranca en moverMotor)
  esp_timer_create_args_t argsTemporizador = {};
  argsTemporizador.callback = &darPaso;
  argsTemporizador.name = "pasos";
  esp_timer_create(&argsTemporizador, &temporizadorPasos);

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
  arrancarServidorWeb();
}

void loop() {
  xSemaphoreTake(candadoEstado, portMAX_DELAY);
  comprobarFinMovimiento();   // para el temporizador al acabar los pulsos
  comprobarRele();            // apaga el rele cuando acaba su tiempo
  xSemaphoreGive(candadoEstado);

  leerBoot();                 // pulsacion de BOOT: cambia entre modo AP y host
  actualizarWifi();           // sigue la busqueda de red en modo host
  actualizarLed();            // parpadeo en AP, doble de rapido buscando, fijo conectado
  delay(1);                   // cede el procesador a las demas tareas (web, WiFi)
}
