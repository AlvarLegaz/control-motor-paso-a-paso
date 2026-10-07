# Control motor paso a paso

Programa para ESP32 con Arduino IDE que controla un motor paso a paso desde una página web. El ESP32 se conecta a la WiFi de casa; si no lo consigue en 15 segundos, crea su propia red ESP32_MOTOR en modo Access Point. Funciona con cualquier driver de entradas STEP y DIR: A4988, DRV8825, TMC2208, TB6600, DM542 y similares.

El montaje probado usa un ESP32 WROOM-32, un driver Pololu DRV8825 y un motor 17HS19-2004S1. Sus hojas de datos y el detalle de todas las conexiones, el ajuste de corriente y el microstepping están en [docu/readme.txt](docu/readme.txt).

## Conexiones

| ESP32 WROOM | Driver | En drivers optoacoplados, tipo TB6600 |
|---|---|---|
| GPIO 33 (D33) | STEP | PUL+ |
| GPIO 32 (D32) | DIR | DIR+ |
| GPIO 25 (D25) | EN | ENA+ |
| GND | GND lógico | PUL-, DIR- y ENA- |

En la placa de desarrollo, D33 y D32 están justo al lado de D35 y D34. Los pines D34, D35, D36 (VP) y D39 (VN) no sirven para el driver: en el ESP32 son solo de entrada. El programa no compila si se ponen en `STEP_PIN` o `DIR_PIN`.

- El ESP32 da señales de 3,3 V. A4988, DRV8825 y TMC2208 las aceptan directamente. Algunos drivers optoacoplados necesitan 5 V: si el motor no responde, revisa la hoja del driver.
- Si el driver tiene entrada ENABLE, déjala habilitada. En el A4988 y el DRV8825 lo está por defecto si no se conecta.
- La alimentación del motor va al driver, nunca al ESP32. Une las masas del ESP32 y del driver.

Los pines se cambian en `STEP_PIN` y `DIR_PIN`, al principio del programa.

## Parámetros

Todos están al principio de `control_motor_paso_a_paso.ino`:

| Parámetro | Valor inicial | Qué hace |
|---|---|---|
| `STEP_PIN` | 33 | GPIO conectado a STEP |
| `DIR_PIN` | 32 | GPIO conectado a DIR |
| `ENABLE_PIN` | 25 | GPIO conectado a EN, para cortar la corriente del motor en reposo |
| `ENABLE_ACTIVO` | LOW | Nivel de EN que activa el driver. LOW en DRV8825 y A4988 |
| `DESACTIVAR_EN_REPOSO` | true | Corta la corriente del motor cuando lleva `REPOSO_MS` parado. Pon `false` si una carga puede mover el eje en reposo |
| `REPOSO_MS` | 500 | Tiempo parado antes de cortar la corriente |
| `ENABLE_ESPERA_MS` | 5 | Espera tras dar corriente antes del primer paso |
| `PULSOS_POR_VUELTA` | 200 | Pulsos para una vuelta. Con microstepping se multiplica: 1/8 son 1600, 1/16 son 3200 |
| `PULSO_US` | 1000 | Microsegundos de STEP en alto y luego en bajo. Un paso dura el doble |
| `LED_PIN` | 2 | LED de estado de la WiFi, el azul de la placa |
| `TIEMPO_CONEXION_MS` | 15000 | Tiempo máximo intentando conectar al router |
| `NOMBRE_RED` | motor | La web se abre también en `http://motor.local` |
| `WIFI_SSID` | ESP32_MOTOR | Nombre de la red propia, si no hay router |
| `WIFI_PASSWORD` | 12345678 | Clave de la red propia, de 8 caracteres como mínimo |
| `WIFI_CANAL` | 6 | Canal de la red propia |
| `WIFI_POTENCIA` | 8,5 dBm | Potencia de la red propia. Baja porque en placas clónicas la red no aparecía a potencia máxima |
| `DIR_DERECHA` | HIGH | Nivel de DIR para girar a la derecha. Cámbialo a LOW si gira al revés |
| `DIR_SETUP_US` | 5 | Espera entre cambiar DIR y el primer pulso |
| `VUELTAS_MAX` | 1000 | Máximo de vueltas por orden |

Con `PULSO_US` a 1000 el motor da 500 pasos por segundo, que son 2,5 vueltas por segundo a 200 pulsos por vuelta. Si vibra sin girar o pierde pasos, sube `PULSO_US`.

## Cargar el programa

1. En Arduino IDE, instala el soporte de ESP32: menú Herramientas, Placa, Gestor de placas, busca "esp32" de Espressif Systems e instálalo. Si no aparece, añade esta dirección en Archivo, Preferencias, "Gestor de URLs adicionales de tarjetas":

   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

2. En la carpeta `control_motor_paso_a_paso`, copia `credenciales.h.ejemplo` como `credenciales.h` y pon el nombre y la clave de tu WiFi. El ESP32 solo usa redes de 2,4 GHz. `credenciales.h` está en `.gitignore` y no se sube al repositorio.
3. Abre `control_motor_paso_a_paso/control_motor_paso_a_paso.ino`.
4. Elige la placa "ESP32 Dev Module" y el puerto del ESP32.
5. Pulsa Subir. Si sale "Wrong boot mode", mantén pulsado BOOT, pulsa y suelta EN, y sigue con BOOT pulsado hasta que salga "Writing at".

No hace falta instalar ninguna librería: `WiFi.h`, `WebServer.h` y `ESPmDNS.h` vienen con el soporte de ESP32.

## Uso

1. Abre el Monitor Serie a 115200 baudios. Al arrancar dice si se ha conectado a la WiFi de casa y muestra la IP.
2. Con el móvil o el PC en la misma WiFi, abre en el navegador `http://motor.local` o esa IP.
3. Si no pudo conectar al router, crea su red propia: conéctate a `ESP32_MOTOR` con la clave `12345678` y abre `http://192.168.4.1`.

El LED azul de la placa indica el estado de la WiFi:

| LED | Estado |
|---|---|
| Fijo | Conectado a la WiFi de casa |
| Parpadeo rápido | Conectando, o se ha perdido la conexión |
| Parpadeo lento | Sin router, con la red propia ESP32_MOTOR |

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
| `GET /estado` | Devuelve el estado en JSON: si se mueve, pulsos pendientes, sentido, pulsos por vuelta y si el motor tiene corriente |

## Cómo se generan los pulsos

Generar los pulsos con un bucle de `delayMicroseconds()` deja el ESP32 ocupado hasta que el motor termina: 10 vueltas a 2 ms por paso son 4 segundos sin atender la web ni el botón Parar.

Una versión anterior los generaba en `loop()` comparando con `micros()`. Funcionaba, pero mientras la web respondía una petición, unos pocos milisegundos, no salían pulsos. El motor se frenaba de golpe y se oía un golpe seco cada segundo, cuando la página consulta el estado.

Ahora `moverMotor()` configura DIR, apunta cuántos pulsos hay que dar y arranca un temporizador del ESP32 (`esp_timer`). El temporizador salta cada `PULSO_US` microsegundos y alterna la salida STEP en `darPaso()`, independientemente de lo que esté haciendo la web. El número de pulsos es siempre exacto. `PULSO_US` debe ser de 50 µs como mínimo, que es lo menos que admite el temporizador.
