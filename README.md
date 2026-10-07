# Control motor paso a paso

Programa para ESP32 con Arduino IDE que controla un motor paso a paso desde una página web. El ESP32 crea su propia red WiFi en modo Access Point, así que no hace falta router. Funciona con cualquier driver de entradas STEP y DIR: A4988, DRV8825, TMC2208, TB6600, DM542 y similares.

## Conexiones

| ESP32 | Driver | En drivers optoacoplados, tipo TB6600 |
|---|---|---|
| GPIO 26 | STEP | PUL+ |
| GPIO 27 | DIR | DIR+ |
| GND | GND lógico | PUL- y DIR- |

- El ESP32 da señales de 3,3 V. A4988, DRV8825 y TMC2208 las aceptan directamente. Algunos drivers optoacoplados necesitan 5 V: si el motor no responde, revisa la hoja del driver.
- Si el driver tiene entrada ENABLE, déjala habilitada. En el A4988 y el DRV8825 lo está por defecto si no se conecta.
- La alimentación del motor va al driver, nunca al ESP32. Une las masas del ESP32 y del driver.

Los pines se cambian en `STEP_PIN` y `DIR_PIN`, al principio del programa.

## Parámetros

Todos están al principio de `control_motor_paso_a_paso.ino`:

| Parámetro | Valor inicial | Qué hace |
|---|---|---|
| `STEP_PIN` | 26 | GPIO conectado a STEP |
| `DIR_PIN` | 27 | GPIO conectado a DIR |
| `PULSOS_POR_VUELTA` | 200 | Pulsos para una vuelta. Con microstepping se multiplica: 1/8 son 1600, 1/16 son 3200 |
| `PULSO_US` | 1000 | Microsegundos de STEP en alto y luego en bajo. Un paso dura el doble |
| `WIFI_SSID` | ESP32_MOTOR | Nombre de la red WiFi |
| `WIFI_PASSWORD` | 12345678 | Clave de la red, de 8 caracteres como mínimo |
| `DIR_DERECHA` | HIGH | Nivel de DIR para girar a la derecha. Cámbialo a LOW si gira al revés |
| `DIR_SETUP_US` | 5 | Espera entre cambiar DIR y el primer pulso |
| `VUELTAS_MAX` | 1000 | Máximo de vueltas por orden |

Con `PULSO_US` a 1000 el motor da 500 pasos por segundo, que son 2,5 vueltas por segundo a 200 pulsos por vuelta. Si vibra sin girar o pierde pasos, sube `PULSO_US`.

## Cargar el programa

1. En Arduino IDE, instala el soporte de ESP32: menú Herramientas, Placa, Gestor de placas, busca "esp32" de Espressif Systems e instálalo. Si no aparece, añade esta dirección en Archivo, Preferencias, "Gestor de URLs adicionales de tarjetas":

   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

2. Abre `control_motor_paso_a_paso/control_motor_paso_a_paso.ino`.
3. Elige la placa "ESP32 Dev Module" y el puerto del ESP32.
4. Pulsa Subir.

No hace falta instalar ninguna librería: `WiFi.h` y `WebServer.h` vienen con el soporte de ESP32.

## Uso

1. Abre el Monitor Serie a 115200 baudios. Al arrancar muestra la IP, normalmente 192.168.4.1.
2. Conecta el móvil, la tablet o el PC a la red `ESP32_MOTOR` con la clave `12345678`.
3. Abre en el navegador `http://192.168.4.1`.

La página muestra los pulsos por vuelta, un campo para el número de vueltas, los botones de girar a cada lado, los de una vuelta a cada lado y un botón Parar. También muestra si el motor está girando y cuántos pulsos le quedan.

El número de pulsos se calcula como vueltas por `PULSOS_POR_VUELTA`, redondeado al pulso más cercano. Con 200 pulsos por vuelta, 0,5 vueltas son 100 pulsos y 2 vueltas son 400. Admite decimales con punto o con coma.

Mientras el motor gira, las órdenes de mover se rechazan con un aviso. Hay que esperar a que termine o pulsar Parar.

## Órdenes HTTP

La página usa estas direcciones, que también se pueden llamar desde otro programa:

| Dirección | Qué hace |
|---|---|
| `GET /` | Página de control |
| `GET /mover?vueltas=1.5&sentido=der` | Gira 1,5 vueltas a la derecha. `sentido` es `der` o `izq` |
| `GET /parar` | Para el motor |
| `GET /estado` | Devuelve el estado en JSON: si se mueve, pulsos pendientes, sentido y pulsos por vuelta |

## Por qué el movimiento no bloquea la web

Generar los pulsos con un bucle de `delayMicroseconds()` deja el ESP32 ocupado hasta que el motor termina: 10 vueltas a 2 ms por paso son 4 segundos sin atender la web ni el botón Parar.

Por eso `moverMotor()` solo configura DIR y apunta cuántos pulsos hay que dar. En cada pasada de `loop()` se atiende la web y, si ya ha pasado `PULSO_US` desde el último cambio, `actualizarMotor()` cambia la salida STEP comparando con `micros()`. El número de pulsos es siempre exacto.

Mientras el ESP32 responde a una petición de la web, que tarda unos pocos milisegundos, el pulso en curso se alarga un poco. El motor nunca va más rápido de lo configurado, solo un instante más lento. A velocidades altas y sin rampa de aceleración eso puede notarse como un pequeño tirón cada segundo, cuando la página consulta el estado. Si molesta, sube `PULSO_US`.
