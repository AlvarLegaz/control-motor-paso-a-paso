CONEXIONES - CONTROL MOTOR PASO A PASO
======================================

Material
  - ESP32 WROOM-32 (placa de desarrollo con conversor USB CH340)
  - Driver Pololu 2133: DRV8825 (hoja: POLOLU-2133.pdf)
  - Motor STEPPERONLINE 17HS19-2004S1 (hoja: 17HS19-2004S1_Full_Datasheet.pdf)
      bipolar, 1,8 grados por paso (200 pasos por vuelta),
      2,0 A por fase, 1,6 ohmios por fase, 3,0 mH, 0,59 Nm
  - Fuente para el motor de 8,2 a 45 V (por ejemplo 12 V o 24 V)
  - Condensador electrolitico de 100 uF (minimo 47 uF), de tension mayor que la fuente


1. ESP32 -> DRV8825 (senales)
-----------------------------

  ESP32              DRV8825        Notas
  -----              -------        -----
  D33 (GPIO 33)      STEP           STEP_PIN en el programa
  D32 (GPIO 32)      DIR            DIR_PIN en el programa
  3V3                RST y SLP      Puentear RST con SLP y unirlos a 3V3.
                                    Si quedan sueltos el driver NO funciona
                                    (los dos llevan resistencia a masa).
  GND                GND            El GND de la esquina, junto a FLT (masa logica)
  D25 (GPIO 25)      EN             ENABLE_PIN en el programa. Corta la
                                    corriente del motor cuando lleva parado
                                    REPOSO_MS (500 ms). Sin este cable el
                                    driver queda siempre habilitado y el motor
                                    siempre con corriente.
  (sin conectar)     M0, M1, M2     Sin conectar = paso completo (200 pulsos/vuelta)
  (sin conectar)     FLT            Salida de fallo, no se usa

  No usar D34, D35, D36 (VP) ni D39 (VN) para STEP o DIR: en el ESP32 son
  solo de entrada. El programa no compila si se ponen.

  El DRV8825 acepta senales de 3,3 V y no necesita alimentacion logica.


1b. ESP32 -> modulo de dos reles
--------------------------------

  ESP32              Modulo         Notas
  -----              ------         -----
  D27 (GPIO 27)      IN1            Rele 1, RELE1_PIN en el programa
  D26 (GPIO 26)      IN2            Rele 2, RELE2_PIN en el programa
  GND                GND
  VIN (5 V del USB)  VCC            Para modulos de 5 V. Si es de 3,3 V, a 3V3

  Los botones "Activar rele 1" y "Activar rele 2" de la web encienden cada
  rele durante el tiempo de su campo (1 s por defecto, RELE_TIEMPO_S) y se
  apaga solo. Maximo RELE_MAX_S (600 s).
  La activacion en serie ("Rele 1 y luego rele 2" o "Rele 2 y luego rele 1")
  enciende un rele durante su tiempo y, cuando se apaga, el otro durante el
  suyo.
  El boton "Realizar disparo" gira el motor las vueltas del campo, espera
  1 s (DISPARO_ESPERA_MS) y enciende los reles segun la opcion elegida: rele
  1, rele 2, 1 y luego 2, o 2 y luego 1.

  No pongas los reles en D14 ni en D15: esos pines dan pulsos durante el
  arranque del ESP32 y el rele chasquearia al encender o reiniciar la placa.

  Muchos modulos con optoacoplador se encienden con IN a nivel BAJO. Si los
  reles se encienden al arrancar el ESP32 y se apagan al pulsar el boton,
  cambia RELE_ACTIVO a LOW en el programa (vale para los dos).
  Con modulos de 5 V activos a nivel bajo, los 3,3 V del ESP32 a veces no
  bastan para apagarlos del todo. Si se queda encendido, alimenta el modulo a
  3,3 V si lo admite o usa un modulo activo a nivel alto.


2. Fuente -> DRV8825 (potencia)
-------------------------------

  Fuente +           VMOT
  Fuente -           GND            El GND junto a VMOT
  Condensador        VMOT y GND     Lo mas cerca posible del driver, con el
                                    negativo (franja) a GND

  La masa de la fuente y la del ESP32 tienen que estar unidas (en la placa
  del driver los dos GND ya estan conectados entre si).

  La fuente del motor NUNCA va al ESP32.


3. Motor -> DRV8825
-------------------

  Cable del motor    Bobina         DRV8825
  ---------------    ------         -------
  Negro  (BLK)       A+             A1
  Verde  (GRN)       A-             A2
  Rojo   (RED)       B+             B1
  Azul   (BLU)       B-             B2

  Si el motor gira al reves de lo que dice la web, se puede cambiar
  DIR_DERECHA en el programa (HIGH por LOW) o intercambiar Negro con Verde.

  NUNCA conectar ni desconectar el motor con el driver alimentado: rompe el
  driver.


3b. Corriente en reposo
-----------------------

  Parado y habilitado, el DRV8825 deja las dos bobinas al 70 % del limite
  de corriente (en paso completo) para mantener el eje fijo. Motor y driver
  se calientan aunque no se muevan.

  Con DESACTIVAR_EN_REPOSO = true (por defecto) el programa pone EN en alto
  a los REPOSO_MS de parar: el motor queda sin corriente y con el eje libre.
  Antes de cada movimiento lo vuelve a habilitar.

  Pon DESACTIVAR_EN_REPOSO = false si el eje lleva una carga que lo pueda
  mover estando parado (un peso vertical, un muelle...): sin corriente se
  desplazaria y el programa no se enteraria.


4. Limite de corriente (potenciometro del DRV8825)
--------------------------------------------------

  Corriente limite = VREF x 2

  VREF se mide entre el punto "ref" (via marcada en la cara de abajo de la
  placa, o el propio cursor del potenciometro) y GND, con el driver
  alimentado y el motor parado.

  Situacion                          Corriente     VREF
  ---------                          ---------     ----
  Sin disipador (recomendado)        1,5 A         0,75 V
  Con disipador y ventilacion        2,0 A         1,00 V   (maximo del motor)

  Sin disipador el DRV8825 no da mas de unos 1,5 A por bobina. El motor
  tiene par de sobra para empezar con 1,0-1,2 A (VREF 0,50-0,60 V).
  El driver se calienta hasta quemar al tocarlo.


5. Microstepping (opcional)
---------------------------

  Si se usa, hay que cambiar PULSOS_POR_VUELTA en el programa.

  M0     M1     M2     Resolucion     PULSOS_POR_VUELTA
  --     --     --     ----------     -----------------
  -      -      -      completo       200
  3V3    -      -      1/2            400
  -      3V3    -      1/4            800
  3V3    3V3    -      1/8            1600
  -      -      3V3    1/16           3200
  3V3    -      3V3    1/32           6400

  (-) = sin conectar


6. Modos WiFi, boton BOOT y LED (LED azul de la placa, GPIO 2)
---------------------------------------------------------------

         ENCENDER
            |
            v
   +------------------+   BOOT (pasados 3 s)   +--------------------+
   |     MODO AP      | ---------------------> |   BUSCANDO RED     |
   |  LED parpadeando |                        |  LED parpadeando   |
   |  ESP32_MOTOR     | <--------------------- |  el doble de rapido|
   |  192.168.4.1     |   BOOT, o ninguna red  |  casa, luego 2a    |
   +------------------+                        +--------------------+
            ^                                            |
            |                BOOT                        | conecta
            |                                            v
            |                                  +--------------------+
            +--------------------------------- |    MODO HOST       |
                                               |    LED fijo        |
                                               |    motor.local     |
                                               +--------------------+

  - Al encender siempre arranca en modo AP: red ESP32_MOTOR, clave
    12345678, web en http://192.168.4.1
  - BOOT no hace nada durante los 3 primeros segundos ni mientras siga
    pulsado desde el arranque.
  - Cada cambio de modo PARA EL MOTOR, CANCELA EL DISPARO Y APAGA LOS DOS
    RELES (y cancela la activacion en serie).
  - En modo host, si se cae la WiFi, el LED vuelve a parpadear rapido mientras
    reconecta solo.


7. Cargar el programa en esta placa
-----------------------------------

  Placa "ESP32 Dev Module", puerto COM del CH340.
  Esta placa no entra sola en modo de carga: mantener pulsado BOOT, pulsar y
  soltar EN, y seguir con BOOT pulsado hasta que salga "Writing at ...".
  Arreglo permanente: condensador de 10 uF entre EN y GND.
