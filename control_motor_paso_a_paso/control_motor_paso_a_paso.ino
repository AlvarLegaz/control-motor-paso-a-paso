/*
  CONTROL DE MOTOR PASO A PASO CON ESP32 POR WIFI
  ===============================================

  El ESP32 se conecta a la WiFi de casa y sirve una pagina web sencilla para
  mover un motor paso a paso con un driver de entradas STEP y DIR (A4988,
  DRV8825, TMC2208, TB6600, DM542...). Si no consigue conectar al router en
  TIEMPO_CONEXION_MS, crea su propia red WiFi (modo Access Point) ESP32_MOTOR.

  USO
    1. Poner el nombre y la clave de la WiFi de casa en credenciales.h (hay un
       ejemplo en credenciales.h.ejemplo). Ese fichero no se sube a GitHub.
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

  POR QUE NO SE USA delayMicroseconds() PARA LOS PULSOS
    Con un bucle de delayMicroseconds() el ESP32 se queda ocupado hasta que el
    motor termina: 10 vueltas a 2 ms por paso son 4 segundos sin atender la web
    (ni el boton Parar). Por eso los pulsos se generan en loop() comparando el
    tiempo con micros(): en cada pasada de loop() se atiende la web y, si ya ha
    pasado PULSO_US desde el ultimo cambio, se cambia la salida STEP.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>     // nombre motor.local (viene con el core ESP32)

// Nombre y clave de la WiFi de casa: WIFI_ROUTER_SSID y WIFI_ROUTER_PASSWORD
#if __has_include("credenciales.h")
#include "credenciales.h"
#else
#error "Falta credenciales.h: copia credenciales.h.ejemplo como credenciales.h y pon tu WiFi."
#endif

// =====================================================================
//  PARAMETROS CONFIGURABLES
// =====================================================================

#define STEP_PIN            33      // GPIO conectado a STEP del driver (D33)
#define DIR_PIN             32      // GPIO conectado a DIR del driver  (D32)

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
#if (STEP_PIN >= 34 && STEP_PIN <= 39) || (DIR_PIN >= 34 && DIR_PIN <= 39)
#error "STEP_PIN y DIR_PIN no pueden ser los GPIO 34 a 39: en el ESP32 son solo de entrada. Usa por ejemplo 33 y 32."
#endif

WebServer server(80);

// Estado del movimiento. moverMotor() lo prepara y actualizarMotor(), que se
// llama en cada pasada de loop(), genera los pulsos sin bloquear.
long pulsosPendientes = 0;        // pulsos que faltan por dar
bool stepEnAlto = false;          // estado actual de la salida STEP
unsigned long ultimoCambio = 0;   // instante (micros) del ultimo cambio de STEP
bool sentidoDerecha = true;       // sentido del movimiento en curso


// ---------------------------------------------------------------------
//  MOTOR
// ---------------------------------------------------------------------

// Prepara un movimiento de 'pulsos' pulsos en el sentido indicado
// (true = derecha, false = izquierda). Los pulsos los genera loop().
void moverMotor(long pulsos, bool direccion) {
  if (pulsos <= 0) return;

  // 1. Configurar DIR
  digitalWrite(DIR_PIN, direccion ? DIR_DERECHA : !DIR_DERECHA);
  delayMicroseconds(DIR_SETUP_US);

  // 2. Encargar el numero exacto de pulsos
  sentidoDerecha = direccion;
  digitalWrite(STEP_PIN, LOW);
  stepEnAlto = false;
  ultimoCambio = micros();
  pulsosPendientes = pulsos;
}

// Detiene el motor inmediatamente.
void pararMotor() {
  pulsosPendientes = 0;
  digitalWrite(STEP_PIN, LOW);
  stepEnAlto = false;
}

// Genera los pulsos pendientes. Cada pulso es STEP en ALTO durante PULSO_US
// y en BAJO durante PULSO_US. Un pulso cuenta como dado al bajar STEP.
void actualizarMotor() {
  if (pulsosPendientes <= 0) return;

  unsigned long ahora = micros();
  if (ahora - ultimoCambio < PULSO_US) return;   // aun no toca
  ultimoCambio = ahora;

  if (!stepEnAlto) {
    digitalWrite(STEP_PIN, HIGH);
    stepEnAlto = true;
  } else {
    digitalWrite(STEP_PIN, LOW);
    stepEnAlto = false;
    pulsosPendientes--;
    if (pulsosPendientes == 0) {
      Serial.println("Movimiento terminado");
    }
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
          : 'parado';
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

// GET /estado  -> {"moviendo":true,"pulsos_pendientes":120,"sentido":"derecha","pulsos_por_vuelta":200}
void ordenEstado() {
  String json = "{\"moviendo\":";
  json += (pulsosPendientes > 0) ? "true" : "false";
  json += ",\"pulsos_pendientes\":";
  json += pulsosPendientes;
  json += ",\"sentido\":\"";
  json += sentidoDerecha ? "derecha" : "izquierda";
  json += "\",\"pulsos_por_vuelta\":";
  json += PULSOS_POR_VUELTA;
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

// Intenta conectar a la WiFi de casa. Devuelve true si lo consigue.
// Mientras lo intenta, el LED parpadea rapido.
bool conectarRouter() {
  WiFi.setHostname(NOMBRE_RED);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_ROUTER_SSID, WIFI_ROUTER_PASSWORD);

  Serial.print("Conectando a ");
  Serial.print(WIFI_ROUTER_SSID);
  bool led = false;
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < TIEMPO_CONEXION_MS) {
    led = !led;
    digitalWrite(LED_PIN, led ? LED_ENCENDIDO : !LED_ENCENDIDO);
    delay(LED_RAPIDO_MS);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
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
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, !LED_ENCENDIDO);

  Serial.println();
  Serial.println("Control motor paso a paso");

  IPAddress ip;
  if (conectarRouter()) {
    modoRouter = true;
    digitalWrite(LED_PIN, LED_ENCENDIDO);
    ip = WiFi.localIP();
    Serial.print("Conectado a la WiFi ");
    Serial.println(WIFI_ROUTER_SSID);
  } else {
    Serial.println("No se pudo conectar al router: se crea la red propia");
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

  // Por defecto handleClient() espera 1 ms cuando no hay peticiones. Eso
  // limitaria la frecuencia de los pulsos, asi que se desactiva.
  server.enableDelay(false);
  server.begin();
}

void loop() {
  server.handleClient();   // atiende la web (no bloquea si no hay peticiones)
  actualizarMotor();       // da el siguiente cambio de STEP si ya toca
  actualizarLed();         // LED fijo si hay WiFi, parpadeando si no
}
