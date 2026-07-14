"""HTTP-pull OTA trigger (v0.24+ firmware).

Serves the built firmware.bin on this PC, then publishes an MQTT command that
tells the device to download and flash it. The device fully deinits BLE and
disables WiFi sleep before pulling, then reboots into the new image — this is
the reliable path; espota push (tools/ota_upload.py) is desk-only fallback.

Usage:
    python tools/ota_pull.py [firmware.bin path]

Defaults to the S3 build. Requires paho-mqtt. Windows firewall must allow
inbound TCP on PORT (one-time rule, see repo docs / -- the script warns if the
device never fetches).
"""

import http.server
import socket
import socketserver
import sys
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt

# Broker settings come from include/secrets.h (gitignored) so no credentials
# live in this file. Parses the #define lines directly.
import re

_SECRETS = Path(__file__).resolve().parent.parent / "include" / "secrets.h"


def _secret(name: str, default: str = "") -> str:
    m = re.search(rf'#define\s+{name}\s+"?([^"\r\n]*)"?', _SECRETS.read_text())
    return m.group(1).strip() if m else default


MQTT_HOST = _secret("MQTT_HOST")
MQTT_PORT = int(_secret("MQTT_PORT", "1883"))
MQTT_USER = _secret("MQTT_USER")
MQTT_PASS = _secret("MQTT_PASS")
CMD_TOPIC = "lm_mini/cmd/ota"
AVAIL_TOPIC = "lm_mini/availability"
PORT = 8070
TIMEOUT_S = 300

# --press publishes the HA-button payload instead of the explicit URL; the
# device then uses its compiled-in OTA_DEFAULT_URL (same server this script
# runs). Needed to update firmware whose cmd buffer predates the URL payload.
args = [a for a in sys.argv[1:] if a != "--press"]
use_press = "--press" in sys.argv[1:]
bin_path = Path(args[0] if args else
                ".pio/build/lm_controller_s3/firmware.bin").resolve()
if not bin_path.is_file():
    sys.exit(f"not found: {bin_path}")

fetched = threading.Event()
back_online = threading.Event()


class BinHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802 — BaseHTTPRequestHandler API
        if self.path != "/firmware.bin":
            self.send_error(404)
            return
        data = bin_path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        print(f"served {len(data)} bytes to {self.client_address[0]}")
        fetched.set()

    def log_message(self, *args):  # quiet default request logging
        pass


def local_ip() -> str:
    """LAN IP as seen from the broker's subnet (no traffic actually sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((MQTT_HOST, 1))
    ip = s.getsockname()[0]
    s.close()
    return ip


def on_message(_c, _u, msg):
    payload = msg.payload.decode(errors="replace")
    print(f"[mqtt] {msg.topic} = {payload}")
    # Device republishes availability online after the post-flash reboot.
    if msg.topic == AVAIL_TOPIC and payload == "online" and fetched.is_set():
        back_online.set()


httpd = socketserver.TCPServer(("0.0.0.0", PORT), BinHandler)
threading.Thread(target=httpd.serve_forever, daemon=True).start()

url = f"http://{local_ip()}:{PORT}/firmware.bin"
print(f"serving {bin_path.name} ({bin_path.stat().st_size} bytes) at {url}")

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.username_pw_set(MQTT_USER, MQTT_PASS)
client.on_message = on_message
client.connect(MQTT_HOST, MQTT_PORT, 30)
client.subscribe(AVAIL_TOPIC)
client.loop_start()
# Retained: the device's MQTT link can flap (BLE scan / WiFi coexistence), and a
# plain QoS0 publish is lost if it lands in a reconnect gap. A retained message
# is delivered by the broker the moment the device (re)subscribes. Cleared right
# after the fetch so the post-flash reconnect can't re-trigger the OTA.
client.publish(CMD_TOPIC, "PRESS" if use_press else url,
               retain=True).wait_for_publish()
print(f"[mqtt] retained trigger published to {CMD_TOPIC}"
      + (" (PRESS -> device default URL)" if use_press else ""))

deadline = time.time() + TIMEOUT_S
got_fetch = fetched.wait(120)
client.publish(CMD_TOPIC, b"", retain=True).wait_for_publish()  # clear retained
if not got_fetch:
    sys.exit("device never fetched the image within 120 s — is it online? "
             "Is inbound TCP 8070 allowed through the Windows firewall?")
if back_online.wait(deadline - time.time()):
    print("SUCCESS: device flashed and reported back online")
else:
    sys.exit("device fetched the image but did not come back online in time — "
             "check its screen/serial (it may still be flashing or have rolled back)")
