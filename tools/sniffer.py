#!/usr/bin/env python3
"""
sniffer.py - conecta al server y loggea TODO crudo. Sin parsear.
Util para descubrir el protocolo real cuando hay multiples clientes.
"""
import socket, sys, threading, time, argparse

ap = argparse.ArgumentParser()
ap.add_argument("--name", required=True)
ap.add_argument("--host", default="yamanote.proxy.rlwy.net")
ap.add_argument("--port", type=int, default=19365)
ap.add_argument("--pos", default="2480,-1666,13.3")
ap.add_argument("--seconds", type=int, default=15)
args = ap.parse_args()

x, y, z = [float(p) for p in args.pos.split(",")]
s = socket.socket()
s.connect((args.host, args.port))
print(f"[{args.name}] connected")
s.sendall(f"JOIN|{args.name}\n".encode())

stop = False
def recv():
    buf = b""
    while not stop:
        try:
            d = s.recv(4096)
        except OSError: return
        if not d: return
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            print(f"[{args.name}] <- {line.decode(errors='replace').strip()}")

t = threading.Thread(target=recv, daemon=True); t.start()

t0 = time.monotonic()
while time.monotonic() - t0 < args.seconds:
    s.sendall(f"POS|{x:.2f}|{y:.2f}|{z:.2f}\n".encode())
    time.sleep(1.0)
stop = True
s.close()
print(f"[{args.name}] done")
