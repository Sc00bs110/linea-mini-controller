#!/usr/bin/env python3
"""Minimal ArduinoOTA uploader — replacement for espota.py, which intermittently
gets no UDP reply on this network while raw invitations answer instantly
(observed 2026-07-04). Protocol: UDP invitation to :3232, device connects back
over TCP and pulls the image with per-chunk acks. No auth (OTA_PASSWORD empty).

Usage: python tools/ota_upload.py [ip] [firmware.bin]
Defaults: linea-mini-c6.local, .pio/build/lm_controller/firmware.bin
"""
import socket, hashlib, os, sys, time

ESP = sys.argv[1] if len(sys.argv) > 1 else "linea-mini-c6.local"
FILE = sys.argv[2] if len(sys.argv) > 2 else \
    os.path.join(os.path.dirname(__file__), "..", ".pio", "build", "lm_controller", "firmware.bin")
# HOST_PORT default 8070 — reuses the "LineaMini OTA pull (TCP 8070)" Windows
# firewall rule; 43231 had no rule and the device's connect-back was dropped.
PORT = 3232
HOST_PORT = int(os.environ.get("OTA_HOST_PORT", "8070"))

size = os.path.getsize(FILE)
md5 = hashlib.md5(open(FILE, "rb").read()).hexdigest()
print(f"target={ESP} size={size} md5={md5}")

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", HOST_PORT))
srv.listen(1)

u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.settimeout(8)
for i in range(6):
    u.sendto(f"0 {HOST_PORT} {size} {md5}\n".encode(), (ESP, PORT))
    try:
        d, _ = u.recvfrom(128)
        print("invitation accepted:", d)
        break
    except socket.timeout:
        print(f"invitation try {i}: no reply")
else:
    sys.exit("FAILED: no UDP reply — is the board up and OTA armed?")

srv.settimeout(20)
try:
    conn, addr = srv.accept()
except socket.timeout:
    sys.exit("FAILED: device never connected back — image too big for the OTA "
             "slot (check Flash%% <= 100 of 2031616) or TCP blocked")
print("device connected from", addr)
conn.settimeout(15)

sent = 0
t0 = time.time()
with open(FILE, "rb") as f:
    while True:
        chunk = f.read(1460)
        if not chunk:
            break
        conn.sendall(chunk)
        sent += len(chunk)
        try:
            conn.recv(32)
        except socket.timeout:
            sys.exit(f"FAILED: no ack at {sent}/{size}")
        if sent % (400 * 1460) < 1460:
            print(f"  {sent}/{size} ({100 * sent // size}%)")

print(f"upload done in {time.time() - t0:.1f}s — device applies and reboots; "
      "verify via wlog :4444 (frame counters restart) or the on-screen version")
conn.close()
