#!/usr/bin/env python3
"""
fakemate.py - simulador de jugador remoto para GTA SA Cliente MP

Conecta al server TCP y emula un user que se mueve en circulo. Permite
probar el tether anti-streaming y la asignacion de peds del cliente
nativo sin depender de otros users reales.

USO BASICO (circulo cerca de la zona de spawn de CJ):
    python3 fakemate.py

OPCIONES:
    --name NOMBRE        nombre del fake remoto (default: amigotest)
    --host HOST          server host (default: yamanote.proxy.rlwy.net)
    --port PORT          server port (default: 19365)
    --center X,Y,Z       centro del circulo (default: 2480,-1666,13.3 = Grove St)
    --radius METROS      radio del circulo (default: 8 = cerca, NO tether)
    --period SEGUNDOS    periodo de la vuelta completa (default: 20)
    --far                modo lejos: centra el circulo 200m al norte para
                         FORZAR tether. Util para validar v0.34.
    --static X,Y,Z       en vez de circulo, queda quieto en este punto

EJEMPLOS:
    # Amigo caminando cerca del CJ (NO tether, deberia ver pos real)
    python3 fakemate.py --name testbro --radius 5

    # Amigo a 200m: FORZAR tether v0.34
    python3 fakemate.py --name testbro --far

    # Amigo parado a 60m fijos (verificar que tether lo trae a 30m)
    python3 fakemate.py --name testbro --static 2540,-1666,13.3

PROTOCOLO usado (segun PROYECTO_CONTEXTO_COMPLETO_v0.33.txt):
    Cliente -> Server:
        JOIN|<name>                 al conectar
        POS|<x>|<y>|<z>             heartbeat cada 1s
    Server -> Cliente (los recvimos pero solo loggeamos):
        ID|<id>
        PLAYERS|id|name|x|y|z[|...]
        CHAT|...
        QUIT|<id>
        PING
"""

import argparse
import math
import socket
import sys
import threading
import time


def parse_xyz(s, name):
    parts = s.split(",")
    if len(parts) != 3:
        raise SystemExit(f"{name} debe ser X,Y,Z (3 numeros separados por coma)")
    try:
        return tuple(float(p) for p in parts)
    except ValueError:
        raise SystemExit(f"{name}: alguno de los valores no es numero")


def recv_thread(sock, my_name):
    """Recibir lineas del server y loggearlas. No hace nada inteligente, solo
    drena el buffer y muestra al user lo que pasa."""
    buf = b""
    while True:
        try:
            data = sock.recv(4096)
        except OSError:
            return
        if not data:
            print("[net] server cerro la conexion")
            return
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.decode(errors="replace").strip()
            if not line:
                continue
            parts = line.split("|")
            cmd = parts[0]
            if cmd == "ID":
                print(f"[<-] ID asignado: {parts[1] if len(parts) > 1 else '?'}")
            elif cmd == "PLAYERS":
                # Imprimir cada remoto (excepto nosotros) que el server reporte
                rest = parts[1:]
                if len(rest) % 5 != 0:
                    print(f"[<-] PLAYERS formato raro ({len(rest)} campos): {line}")
                    continue
                for i in range(0, len(rest), 5):
                    rid, rname, rx, ry, rz = rest[i:i+5]
                    if rname == my_name:
                        continue  # echo de nosotros mismos
                    print(f"[<-] PLAYERS id={rid} name={rname} pos=({rx},{ry},{rz})")
            elif cmd == "CHAT":
                print(f"[<-] CHAT: {'|'.join(parts[1:])}")
            elif cmd == "QUIT":
                print(f"[<-] QUIT id={parts[1] if len(parts) > 1 else '?'}")
            elif cmd == "PING":
                pass  # silencio
            else:
                print(f"[<-] {line}")


def main():
    ap = argparse.ArgumentParser(
        description="Simulador de jugador remoto para GTASA Cliente MP",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--name", default="amigotest")
    ap.add_argument("--host", default="yamanote.proxy.rlwy.net")
    ap.add_argument("--port", type=int, default=19365)
    ap.add_argument("--center", default="2480,-1666,13.3",
                    help="Centro del circulo X,Y,Z")
    ap.add_argument("--radius", type=float, default=8.0)
    ap.add_argument("--period", type=float, default=20.0,
                    help="Segundos para completar una vuelta")
    ap.add_argument("--far", action="store_true",
                    help="Centra el circulo 200m al norte del center para forzar tether")
    ap.add_argument("--static", default=None,
                    help="Quedarse quieto en X,Y,Z (ignora circulo)")
    args = ap.parse_args()

    cx, cy, cz = parse_xyz(args.center, "--center")
    if args.far:
        # Mover 200m al norte para garantizar > kTetherDistance del cliente
        cy += 200.0
        print(f"[cfg] modo FAR: centro movido a ({cx},{cy},{cz}) (200m N)")

    if args.static:
        sx, sy, sz = parse_xyz(args.static, "--static")
        print(f"[cfg] modo STATIC: pos fija ({sx},{sy},{sz})")
    else:
        sx = sy = sz = None
        print(f"[cfg] modo CIRCLE: centro=({cx},{cy},{cz}) radio={args.radius} periodo={args.period}s")

    print(f"[cfg] name={args.name} server={args.host}:{args.port}")

    # Conectar
    print(f"[net] conectando a {args.host}:{args.port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10.0)
    sock.connect((args.host, args.port))
    sock.settimeout(None)
    print("[net] conectado")

    # JOIN
    join = f"JOIN|{args.name}\n"
    sock.sendall(join.encode())
    print(f"[->] {join.strip()}")

    # Hilo de recepcion
    t = threading.Thread(target=recv_thread, args=(sock, args.name), daemon=True)
    t.start()

    # Loop de envio de POS (1Hz)
    start = time.monotonic()
    try:
        while True:
            now = time.monotonic()
            if sx is not None:
                x, y, z = sx, sy, sz
            else:
                # Circulo en el plano XY alrededor de (cx,cy), z fijo en cz
                phase = ((now - start) / args.period) * 2.0 * math.pi
                x = cx + args.radius * math.cos(phase)
                y = cy + args.radius * math.sin(phase)
                z = cz

            msg = f"POS|{x:.2f}|{y:.2f}|{z:.2f}\n"
            try:
                sock.sendall(msg.encode())
            except OSError as e:
                print(f"[net] send fallo: {e}")
                break
            # log compacto
            print(f"[->] POS|{x:.1f}|{y:.1f}|{z:.1f}")
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\n[ctrl-c] cerrando")
    finally:
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        sock.close()


if __name__ == "__main__":
    sys.exit(main() or 0)
