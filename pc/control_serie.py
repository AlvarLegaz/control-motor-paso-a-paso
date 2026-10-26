"""
Control del motor paso a paso desde el PC por el cable USB (puerto serie).

El ESP32 acepta por el puerto serie las mismas ordenes que la web, una por
linea (/estado, /mover?vueltas=0.25&sentido=der...), y contesta con una linea
"> codigo texto". Las demas lineas son los mensajes del Monitor Serie.

Uso:
  py control_serie.py                      ventana de control
  py control_serie.py --puerto COM3        lo mismo, con el puerto indicado
  py control_serie.py --orden /estado      envia una orden y muestra la respuesta
  py control_serie.py --prueba             prueba de rendimiento (el motor gira)

Necesita pyserial:  py -m pip install pyserial
Cierra el Monitor Serie de Arduino IDE antes: el puerto solo lo puede usar uno.
"""

import argparse
import json
import queue
import sys
import threading
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("Falta pyserial: instala con  py -m pip install pyserial")

BAUDIOS = 115200


def buscar_puerto():
    """Puerto del ESP32: el primero con un conversor USB-serie conocido."""
    conocidos = ("CH340", "CP210", "USB-SERIAL", "USB Serial", "UART")
    puertos = list(serial.tools.list_ports.comports())
    for p in puertos:
        if any(c.lower() in (p.description or "").lower() for c in conocidos):
            return p.device
    return puertos[0].device if puertos else None


class Placa:
    """Conexion con el ESP32. Un hilo lee las lineas: las que empiezan por
    '> ' son respuestas a las ordenes y el resto se pasa a 'al_recibir'."""

    def __init__(self, puerto, al_recibir=print):
        self.al_recibir = al_recibir
        self.respuestas = queue.Queue()
        self.candado = threading.Lock()   # una orden cada vez
        self.puerto = serial.Serial()
        self.puerto.port = puerto
        self.puerto.baudrate = BAUDIOS
        self.puerto.timeout = 0.2
        # Sin tocar DTR y RTS al abrir: en las placas ESP32 reinician la placa
        self.puerto.dtr = False
        self.puerto.rts = False
        self.puerto.open()
        self.abierto = True
        threading.Thread(target=self._leer, daemon=True).start()

    def _leer(self):
        resto = b""
        while self.abierto:
            try:
                datos = self.puerto.read(256)
            except Exception:   # puerto cerrado o placa desenchufada
                if self.abierto:
                    self.al_recibir("[pc] se ha perdido la conexion con la placa")
                return
            if not datos:
                continue
            resto += datos
            *lineas, resto = resto.split(b"\n")
            for linea in lineas:
                texto = linea.decode("utf-8", "replace").rstrip("\r")
                if texto.startswith("> "):
                    self.respuestas.put(texto[2:])
                else:
                    self.al_recibir(texto)

    def orden(self, url, espera=2.0):
        """Envia una orden y devuelve (codigo, texto). codigo 0 = sin respuesta."""
        with self.candado:
            while not self.respuestas.empty():
                self.respuestas.get_nowait()
            self.puerto.write((url + "\n").encode("utf-8"))
            try:
                linea = self.respuestas.get(timeout=espera)
            except queue.Empty:
                return 0, "Sin respuesta de la placa"
        codigo, _, texto = linea.partition(" ")
        return int(codigo) if codigo.isdigit() else 0, texto

    def json(self, url):
        codigo, texto = self.orden(url)
        return json.loads(texto) if codigo == 200 else None

    def cerrar(self):
        self.abierto = False
        self.puerto.close()


# ---------------------------------------------------------------------
#  PRUEBA DE RENDIMIENTO
# ---------------------------------------------------------------------

def prueba(placa, velocidades=(25, 100, 200, 300), segundos=3.0):
    """Gira el motor a cada velocidad unos 'segundos' y mide en la placa la
    regularidad de los pulsos STEP, el bucle principal y la memoria."""
    estado = placa.json("/estado")
    if estado is None:
        print("La placa no responde a /estado")
        return
    ppv = estado["pulsos_por_vuelta"]
    rpm_antes = estado["rpm"]
    print(f"{'rpm':>5} {'medio periodo':>14} {'cambios STEP':>13} {'desvio max':>11} "
          f"{'irregulares':>12} {'bucle max':>10} {'memoria min':>12}")
    try:
        for rpm in velocidades:
            placa.orden(f"/velocidad?rpm={rpm}")
            placa.orden("/diagnostico?reiniciar=1")
            vueltas = min(rpm / 60 * segundos, 20)
            codigo, texto = placa.orden(f"/mover?vueltas={vueltas:.4f}&sentido=der")
            if codigo != 200:
                print(f"{rpm:>5}  no se pudo mover: {texto}")
                continue
            limite = time.time() + segundos * 2 + 3
            while time.time() < limite:
                time.sleep(0.25)
                e = placa.json("/estado")
                if e and not e["moviendo"]:
                    break
            d = placa.json("/diagnostico")
            medio = 30_000_000 // (rpm * ppv)
            print(f"{rpm:>5} {medio:>11} us {d['pasos_medidos']:>13} "
                  f"{d['paso_desvio_max_us']:>8} us {d['pasos_irregulares']:>12} "
                  f"{d['bucle_max_us'] / 1000:>7.1f} ms {d['memoria_minima']:>10} B")
    finally:
        placa.orden("/parar")
        placa.orden(f"/velocidad?rpm={rpm_antes}")


# ---------------------------------------------------------------------
#  VENTANA DE CONTROL
# ---------------------------------------------------------------------

def ventana(puerto):
    import tkinter as tk
    from tkinter import ttk, messagebox

    raiz = tk.Tk()
    raiz.title(f"Control motor paso a paso ({puerto})")
    raiz.minsize(520, 560)

    registro = queue.Queue()   # lineas del Monitor Serie, se pintan en el hilo de la ventana
    try:
        placa = Placa(puerto, al_recibir=registro.put)
    except serial.SerialException as e:
        messagebox.showerror("Puerto serie", f"No se pudo abrir {puerto}:\n{e}\n\n"
                             "Cierra el Monitor Serie de Arduino IDE y prueba de nuevo.")
        raiz.destroy()
        return

    mensaje = tk.StringVar(value="")

    def enviar(url):
        def trabajo():
            codigo, texto = placa.orden(url)
            raiz.after(0, mensaje.set, texto if codigo else "Sin respuesta de la placa")
        threading.Thread(target=trabajo, daemon=True).start()

    marco = ttk.Frame(raiz, padding=10)
    marco.pack(fill="both", expand=True)

    # Motor
    motor = ttk.LabelFrame(marco, text="Motor", padding=8)
    motor.pack(fill="x")
    rpm = tk.IntVar(value=25)
    avance = tk.IntVar(value=90)
    retroceso = tk.IntVar(value=0)
    for fila, (texto, var, desde, hasta) in enumerate((
            ("Velocidad (rpm)", rpm, 10, 300),
            ("Ángulo de avance (°)", avance, 0, 360),
            ("Ángulo de retroceso (°)", retroceso, 0, 360))):
        ttk.Label(motor, text=texto).grid(row=fila, column=0, sticky="w")
        escala = tk.Scale(motor, from_=desde, to=hasta, orient="horizontal", variable=var,
                          length=300, resolution=5 if var is rpm else 1)
        escala.grid(row=fila, column=1, sticky="we")
        if var is rpm:
            escala.bind("<ButtonRelease-1>", lambda e: enviar(f"/velocidad?rpm={rpm.get()}"))
    botones = ttk.Frame(motor)
    botones.grid(row=3, column=0, columnspan=2, pady=(6, 0), sticky="we")
    ttk.Button(botones, text="Retroceder",
               command=lambda: enviar(f"/mover?vueltas={retroceso.get() / 360:.4f}&sentido=izq")
               ).pack(side="left", expand=True, fill="x")
    ttk.Button(botones, text="Avanzar",
               command=lambda: enviar(f"/mover?vueltas={avance.get() / 360:.4f}&sentido=der")
               ).pack(side="left", expand=True, fill="x")
    ttk.Button(botones, text="PARAR", command=lambda: enviar("/parar")
               ).pack(side="left", expand=True, fill="x")

    # Reles y disparo
    reles = ttk.LabelFrame(marco, text="Relés y disparo", padding=8)
    reles.pack(fill="x", pady=(8, 0))
    s1 = tk.StringVar(value="1")
    s2 = tk.StringVar(value="1")
    modo = tk.StringVar(value="1")
    ttk.Label(reles, text="Relé 1 (s)").grid(row=0, column=0, sticky="w")
    ttk.Entry(reles, textvariable=s1, width=6).grid(row=0, column=1)
    ttk.Button(reles, text="Activar relé 1",
               command=lambda: enviar(f"/rele?n=1&segundos={s1.get()}")).grid(row=0, column=2, sticky="we")
    ttk.Label(reles, text="Relé 2 (s)").grid(row=1, column=0, sticky="w")
    ttk.Entry(reles, textvariable=s2, width=6).grid(row=1, column=1)
    ttk.Button(reles, text="Activar relé 2",
               command=lambda: enviar(f"/rele?n=2&segundos={s2.get()}")).grid(row=1, column=2, sticky="we")
    modos = ttk.Frame(reles)
    modos.grid(row=2, column=0, columnspan=3, pady=(6, 0), sticky="w")
    for valor, texto in (("1", "Relé 1"), ("2", "Relé 2"), ("12", "1 y luego 2"), ("21", "2 y luego 1")):
        ttk.Radiobutton(modos, text=texto, value=valor, variable=modo).pack(side="left")

    def disparar():
        enviar(f"/disparo?vueltas={avance.get() / 360:.4f}&sentido=der"
               f"&retroceso={retroceso.get() / 360:.4f}&modo={modo.get()}"
               f"&segundos1={s1.get()}&segundos2={s2.get()}")
    ttk.Button(reles, text="Realizar disparo", command=disparar).grid(
        row=3, column=0, columnspan=3, sticky="we", pady=(6, 0))
    ttk.Button(reles, text="Serie 1 → 2", command=lambda: enviar(
        f"/serie?orden=12&segundos1={s1.get()}&segundos2={s2.get()}")).grid(row=4, column=0, columnspan=2, sticky="we")
    ttk.Button(reles, text="Serie 2 → 1", command=lambda: enviar(
        f"/serie?orden=21&segundos1={s1.get()}&segundos2={s2.get()}")).grid(row=4, column=2, sticky="we")
    reles.columnconfigure(2, weight=1)

    # Estado y diagnostico
    estado = ttk.LabelFrame(marco, text="Estado", padding=8)
    estado.pack(fill="x", pady=(8, 0))
    texto_estado = tk.StringVar(value="-")
    texto_diag = tk.StringVar(value="-")
    ttk.Label(estado, textvariable=texto_estado, justify="left").pack(anchor="w")
    ttk.Label(estado, textvariable=texto_diag, justify="left", foreground="#555").pack(anchor="w")
    ttk.Label(estado, textvariable=mensaje, foreground="#1565c0", wraplength=480).pack(anchor="w")

    # Monitor Serie
    monitor = tk.Text(marco, height=10, font=("Consolas", 9))
    monitor.pack(fill="both", expand=True, pady=(8, 0))

    rpm_leida = [False]

    def leer_estado():
        while placa.abierto:
            e = placa.json("/estado")
            d = placa.json("/diagnostico")
            if e:
                if not rpm_leida[0]:
                    raiz.after(0, rpm.set, e["rpm"])
                    rpm_leida[0] = True
                if e["moviendo"]:
                    t = f"Motor: girando a la {e['sentido']}, quedan {e['pulsos_pendientes']} pulsos"
                else:
                    t = "Motor: parado, " + ("con corriente" if e["con_corriente"] else "sin corriente")
                t += f" · {e['rpm']} rpm"
                for n, clave in ((1, "rele"), (2, "rele2")):
                    t += f"\nRelé {n}: " + (f"encendido, quedan {e[clave + '_ms_restantes'] / 1000:.1f} s"
                                            if e[clave] else "apagado")
                if e["disparo"]:
                    t += f"\nDisparo: {e['disparo']} (modo {e['disparo_modo']})"
                raiz.after(0, texto_estado.set, t)
            else:
                raiz.after(0, texto_estado.set, "Sin respuesta de la placa")
            if d:
                raiz.after(0, texto_diag.set,
                           f"WiFi {d['modo_wifi']} ({d['dispositivos']} disp.) · memoria libre "
                           f"{d['memoria_libre'] // 1024} KB, mínima {d['memoria_minima'] // 1024} KB · "
                           f"pulsos: desvío máx {d['paso_desvio_max_us']} µs")
            time.sleep(1)

    def pintar_registro():
        while not registro.empty():
            monitor.insert("end", registro.get_nowait() + "\n")
            monitor.see("end")
        raiz.after(100, pintar_registro)

    def cerrar():
        placa.cerrar()
        raiz.destroy()

    threading.Thread(target=leer_estado, daemon=True).start()
    pintar_registro()
    raiz.protocol("WM_DELETE_WINDOW", cerrar)
    raiz.mainloop()


def main():
    argumentos = argparse.ArgumentParser(description="Control del motor por el puerto serie")
    argumentos.add_argument("--puerto", help="puerto serie (por defecto, el primero con un ESP32)")
    argumentos.add_argument("--orden", action="append", help="orden a enviar, por ejemplo /estado")
    argumentos.add_argument("--prueba", action="store_true", help="prueba de rendimiento (el motor gira)")
    argumentos.add_argument("--rpm", type=int, nargs="+", default=[25, 100, 200, 300],
                            help="velocidades de la prueba")
    argumentos.add_argument("--monitor", type=float, metavar="S",
                            help="muestra el Monitor Serie durante S segundos")
    a = argumentos.parse_args()

    puerto = a.puerto or buscar_puerto()
    if not puerto:
        sys.exit("No se encuentra ningun puerto serie. ¿Esta la placa conectada?")

    if not (a.orden or a.prueba or a.monitor):
        ventana(puerto)
        return

    placa = Placa(puerto, al_recibir=lambda t: print("   ", t) if a.monitor else None)
    try:
        time.sleep(0.3)
        for url in a.orden or []:
            codigo, texto = placa.orden(url)
            print(f"{url} -> {codigo} {texto}")
        if a.prueba:
            prueba(placa, a.rpm)
        if a.monitor:
            time.sleep(a.monitor)
    finally:
        placa.cerrar()


if __name__ == "__main__":
    main()
