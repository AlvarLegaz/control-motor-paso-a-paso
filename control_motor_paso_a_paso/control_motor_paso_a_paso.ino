/*
  CONTROL DE MOTOR PASO A PASO CON ESP32 POR WIFI
  ===============================================

  El ESP32 se conecta a una WiFi y sirve una pagina web sencilla para mover
  un motor paso a paso con un driver de entradas STEP y DIR (A4988, DRV8825,
  TMC2208, TB6600, DM542...). Prueba primero la WiFi de casa y despues una
  segunda red, cada una durante TIEMPO_CONEXION_MS. Si no conecta a ninguna,
  crea su propia red WiFi (modo Access Point) ESP32_MOTOR.

  USO
    1. Poner nombre y clave de las WiFi en credenciales.h (hay un ejemplo en
       credenciales.h.ejemplo). Ese fichero no se sube a GitHub.
    2. Cargar este programa en el ESP32 (placa "ESP32 Dev Module").
    3. Abrir el Monitor Serie a 115200 baudios: muestra la IP.
    4. Con el movil o el PC en la misma WiFi, abrir http://motor.local o la IP.
       Si el ESP32 creo su propia red: conectarse a ESP32_MOTOR (clave
       12345678) y abrir http://192.168.4.1

  CONEXIONES (con los valores por defecto de abajo)

    ESP32 (WROOM)       Driver
    -----               ------
    GPIO 33  (STEP_PIN) STEP   (en drivers optoacoplados tipo TB6600: PUL+)
    GPIO 32  (DIR_PIN)  DIR    (en drivers optoacoplados tipo TB6600: DIR+)
    GPIO 25  (ENABLE_PIN) EN   corta la corriente del motor en reposo
                               (en drivers optoacoplados tipo TB6600: ENA+)
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
#include <WebServer.h>
#include <ESPmDNS.h>     // nombre motor.local (viene con el core ESP32)
#include "esp_timer.h"   // temporizador de los pulsos (viene con el core ESP32)

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
//   parpadeo rapido = sin conexion al router (conectando o se ha perdido)
//   fijo            = conectado al router
//   parpadeo lento  = sin router, con la red propia ESP32_MOTOR
#define LED_PIN             2
#define LED_ENCENDIDO       HIGH    // cambia a LOW si tu LED se enciende al reves
#define LED_RAPIDO_MS       200     // medio periodo del parpadeo rapido
#define LED_LENTO_MS        1000    // medio periodo del parpadeo lento

// Segundos maximos intentando conectar a la WiFi de casa antes de crear la
// red propia.
#define TIEMPO_CONEXION_MS  15000

// Nombre en la red: la web se abre tambien en http://motor.local
#define NOMBRE_RED          "motor"

// Red propia (Access Point) si no hay router
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

// =====================================================================

// Los GPIO 34 a 39 del ESP32 son solo de entrada: no pueden mover el driver.
#if (STEP_PIN >= 34 && STEP_PIN <= 39) || (DIR_PIN >= 34 && DIR_PIN <= 39) || (ENABLE_PIN >= 34 && ENABLE_PIN <= 39)
#error "STEP_PIN, DIR_PIN y ENABLE_PIN no pueden ser los GPIO 34 a 39: en el ESP32 son solo de entrada. Usa por ejemplo 33, 32 y 25."
#endif

// El temporizador del ESP32 no admite periodos menores de 50 us.
#if PULSO_US < 50
#error "PULSO_US debe ser de 50 microsegundos o mas."
#endif

WebServer server(80);

// Estado del movimiento. moverMotor() lo prepara y el temporizador da los
// pulsos. 'volatile' porque el temporizador corre en otra tarea del ESP32.
volatile long pulsosPendientes = 0;   // pulsos que faltan por dar
volatile bool stepEnAlto = false;     // estado actual de la salida STEP
bool sentidoDerecha = true;           // sentido del movimiento en curso
bool temporizadorEnMarcha = false;
bool driverActivo = false;            // true = bobinas con corriente
unsigned long finMovimiento = 0;      // instante (millis) en que paro el motor

esp_timer_handle_t temporizadorPasos;
portMUX_TYPE candadoMotor = portMUX_INITIALIZER_UNLOCKED;  // evita que el
                         // temporizador y la web toquen el estado a la vez


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
  Serial.println("Motor en reposo: sin corriente");
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
    Serial.println("Movimiento terminado");
  }
  if (DESACTIVAR_EN_REPOSO && driverActivo && !temporizadorEnMarcha &&
      millis() - finMovimiento >= REPOSO_MS) {
    desactivarDriver();
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
    <button class="parar" onclick="orden('/parar')">Parar</button>
  </div>

  <div class="caja">
    Estado: <span id="estado">-</span><br>
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

  // Consulta el estado del motor cada segundo
  function leerEstado() {
    fetch('/estado')
      .then(r => r.json())
      .then(e => {
        document.getElementById('estado').textContent = e.moviendo
          ? 'girando a la ' + e.sentido + ', quedan ' + e.pulsos_pendientes + ' pulsos'
          : (e.con_corriente ? 'parado, con corriente' : 'parado, sin corriente');
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

// GET /  -> pagina de control
void paginaPrincipal() {
  String html = PAGINA;
  html.replace("%PPV%", String(PULSOS_POR_VUELTA));
  server.send(200, "text/html; charset=utf-8", html);
}

// GET /mover?vueltas=1.5&sentido=der  (sentido: der o izq)
void ordenMover() {
  if (pulsosPendientes > 0) {
    server.send(409, "text/plain; charset=utf-8",
                "El motor está girando. Espera a que termine o pulsa Parar.");
    return;
  }

  String textoVueltas = server.arg("vueltas");
  textoVueltas.replace(",", ".");          // admite 0,5 ademas de 0.5
  float vueltas = textoVueltas.toFloat();
  String sentido = server.arg("sentido");

  if (vueltas <= 0 || vueltas > VUELTAS_MAX) {
    String error = "Número de vueltas no válido (entre 0 y ";
    error += VUELTAS_MAX;
    error += ").";
    server.send(400, "text/plain; charset=utf-8", error);
    return;
  }
  if (sentido != "der" && sentido != "izq") {
    server.send(400, "text/plain; charset=utf-8", "Sentido no válido.");
    return;
  }

  // pulsos = vueltas * PULSOS_POR_VUELTA, redondeado al pulso mas cercano
  long pulsos = (long)(vueltas * PULSOS_POR_VUELTA + 0.5f);
  if (pulsos < 1) {
    server.send(400, "text/plain; charset=utf-8", "Son menos de un pulso.");
    return;
  }

  bool derecha = (sentido == "der");
  moverMotor(pulsos, derecha);

  String mensaje = "Girando ";
  mensaje += String(vueltas, 2);
  mensaje += derecha ? " vueltas a la derecha (" : " vueltas a la izquierda (";
  mensaje += pulsos;
  mensaje += " pulsos)";
  Serial.println(mensaje);
  server.send(200, "text/plain; charset=utf-8", mensaje);
}

// GET /parar
void ordenParar() {
  pararMotor();
  Serial.println("Parado desde la web");
  server.send(200, "text/plain; charset=utf-8", "Motor parado.");
}

// GET /estado  -> {"moviendo":true,"pulsos_pendientes":120,"sentido":"derecha","pulsos_por_vuelta":200,"con_corriente":true}
void ordenEstado() {
  String json = "{\"moviendo\":";
  json += (pulsosPendientes > 0) ? "true" : "false";
  json += ",\"pulsos_pendientes\":";
  json += pulsosPendientes;
  json += ",\"sentido\":\"";
  json += sentidoDerecha ? "derecha" : "izquierda";
  json += "\",\"pulsos_por_vuelta\":";
  json += PULSOS_POR_VUELTA;
  json += ",\"con_corriente\":";
  json += driverActivo ? "true" : "false";
  json += "}";
  server.send(200, "application/json", json);
}

void paginaNoEncontrada() {
  server.send(404, "text/plain; charset=utf-8", "No encontrado");
}


// ---------------------------------------------------------------------
//  WIFI
// ---------------------------------------------------------------------

bool modoRouter = false;   // true si se trabaja con la WiFi de casa

// Pone el LED segun el estado de la WiFi. Se llama en cada pasada de loop()
// y no bloquea.
void actualizarLed() {
  static unsigned long ultimoCambio = 0;
  static bool encendido = false;

  if (modoRouter && WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, LED_ENCENDIDO);          // conectado: fijo
    encendido = true;
    return;
  }
  unsigned long medioPeriodo = modoRouter ? LED_RAPIDO_MS : LED_LENTO_MS;
  if (millis() - ultimoCambio >= medioPeriodo) {
    ultimoCambio = millis();
    encendido = !encendido;
    digitalWrite(LED_PIN, encendido ? LED_ENCENDIDO : !LED_ENCENDIDO);
  }
}

// Redes WiFi que se prueban en orden, con su clave (en credenciales.h).
// Si no conecta a ninguna, se crea la red propia ESP32_MOTOR.
const char* REDES_SSID[]  = { WIFI_ROUTER_SSID,     WIFI_ROUTER2_SSID };
const char* REDES_CLAVE[] = { WIFI_ROUTER_PASSWORD, WIFI_ROUTER2_PASSWORD };
const int   NUM_REDES = sizeof(REDES_SSID) / sizeof(REDES_SSID[0]);
const char* redConectada = "";

// Intenta conectar a una red durante TIEMPO_CONEXION_MS. Devuelve true si lo
// consigue. Mientras lo intenta, el LED parpadea rapido.
bool conectarRed(const char* ssid, const char* clave) {
  if (ssid[0] == '\0') return false;   // red sin configurar
  WiFi.disconnect();
  WiFi.begin(ssid, clave);

  Serial.print("Conectando a ");
  Serial.print(ssid);
  bool led = false;
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < TIEMPO_CONEXION_MS) {
    led = !led;
    digitalWrite(LED_PIN, led ? LED_ENCENDIDO : !LED_ENCENDIDO);
    delay(LED_RAPIDO_MS);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " conectado" : " no disponible");
  return WiFi.status() == WL_CONNECTED;
}

// Prueba las redes de REDES_SSID en orden. Devuelve true si conecta a alguna.
bool conectarRouter() {
  WiFi.setHostname(NOMBRE_RED);
  WiFi.mode(WIFI_STA);
  for (int i = 0; i < NUM_REDES; i++) {
    if (conectarRed(REDES_SSID[i], REDES_CLAVE[i])) {
      redConectada = REDES_SSID[i];
      return true;
    }
  }
  return false;
}

// Crea la red propia ESP32_MOTOR (modo Access Point).
void crearRedPropia() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(WIFI_SSID, WIFI_PASSWORD, WIFI_CANAL)) {
    Serial.println("ERROR: no se pudo crear la red WiFi (la clave debe tener al menos 8 caracteres)");
  }
  WiFi.setTxPower(WIFI_POTENCIA);
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

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, !LED_ENCENDIDO);

  // Temporizador que da los pulsos (se arranca en moverMotor)
  esp_timer_create_args_t argsTemporizador = {};
  argsTemporizador.callback = &darPaso;
  argsTemporizador.name = "pasos";
  esp_timer_create(&argsTemporizador, &temporizadorPasos);

  Serial.println();
  Serial.println("Control motor paso a paso");

  IPAddress ip;
  if (conectarRouter()) {
    modoRouter = true;
    digitalWrite(LED_PIN, LED_ENCENDIDO);
    ip = WiFi.localIP();
    Serial.print("Conectado a la WiFi ");
    Serial.println(redConectada);
  } else {
    Serial.println("No se pudo conectar a ninguna red: se crea la red propia");
    crearRedPropia();
    ip = WiFi.softAPIP();
    Serial.print("Red WiFi propia: ");
    Serial.print(WIFI_SSID);
    Serial.print(" (clave ");
    Serial.print(WIFI_PASSWORD);
    Serial.println(")");
  }

  Serial.print("IP: ");
  Serial.println(ip);
  Serial.print("Abre en el navegador: http://");
  Serial.println(ip);
  if (MDNS.begin(NOMBRE_RED)) {
    MDNS.addService("http", "tcp", 80);
    Serial.print("o tambien: http://");
    Serial.print(NOMBRE_RED);
    Serial.println(".local");
  }
  Serial.print("Pulsos por vuelta: ");
  Serial.println(PULSOS_POR_VUELTA);

  server.on("/", HTTP_GET, paginaPrincipal);
  server.on("/mover", HTTP_GET, ordenMover);
  server.on("/parar", HTTP_GET, ordenParar);
  server.on("/estado", HTTP_GET, ordenEstado);
  server.onNotFound(paginaNoEncontrada);
  server.begin();
}

void loop() {
  server.handleClient();      // atiende la web
  comprobarFinMovimiento();   // para el temporizador al acabar los pulsos
  actualizarLed();            // LED fijo si hay WiFi, parpadeando si no
}
