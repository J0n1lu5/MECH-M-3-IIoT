"""Nachbildung der HTTP-API des ESP32 (src/esp32_firmware/src/main.cpp), damit die Tests ohne Board laufen.

Von Hand starten (z.B. für curl):  python tests/mock_device.py 8080
"""

import json
import random
import sys
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_INTERVAL_SECONDS = 4294967  # wie in main.cpp


class Device:
    device_id = "nockenberg-pluder-esp32-01"
    interval = 60
    started = time.monotonic()
    last_reading = None
    last_telemetry = None
    last_humidity = None

    @classmethod
    def measure_if_due(cls):
        """Wie loop() in der Firmware: messen, sobald das Intervall abgelaufen ist."""
        now = time.monotonic()
        if cls.last_reading is None or now - cls.last_reading >= cls.interval:
            cls.last_reading = now
            cls.last_telemetry = {
                "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "value": round(random.uniform(20, 25), 1),
                "unit": "°C",
            }
            cls.last_humidity = float(random.randint(40, 60))


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_json(self, code, payload, allow=None):
        body = json.dumps(payload, ensure_ascii=False).encode()
        self.send_response(code)
        if allow:
            self.send_header("Allow", allow)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def handle_request(self):
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        path, method = self.path.split("?")[0], self.command
        Device.measure_if_due()
        if path == "/config" and method == "GET":
            self.send_json(200, {"device_id": Device.device_id, "reading_interval_seconds": Device.interval})
        elif path == "/config" and method == "POST":
            self.post_config(body)
        elif path == "/config":
            self.send_json(405, {"error": "Nur GET, POST erlaubt"}, allow="GET, POST")
        elif path == "/status" and method == "GET":
            self.send_json(200, {
                "device_id": Device.device_id,
                "uptime_seconds": int(time.monotonic() - Device.started),
                "wifi_connected": True,
                "mqtt_connected": True,
                "last_telemetry": Device.last_telemetry,
                "last_humidity_percent": Device.last_humidity,
            })
        elif path == "/status":
            self.send_json(405, {"error": "Nur GET erlaubt"}, allow="GET")
        else:
            self.send_json(404, {"error": "Unbekannter Pfad"})

    def post_config(self, body):
        try:
            interval = json.loads(body)["reading_interval_seconds"]
        except (ValueError, TypeError, KeyError):
            return self.send_json(400, {"error": "Fehlerhaftes JSON"})
        # wie in main.cpp: jede ganze JSON-Zahl >= 2, keine bool-Werte, große Werte werden begrenzt
        if isinstance(interval, bool) or not isinstance(interval, (int, float)) or interval < 2 or interval % 1:
            return self.send_json(400, {"error": "reading_interval_seconds muss eine Ganzzahl >= 2 sein"})
        Device.interval = min(int(interval), MAX_INTERVAL_SECONDS)
        self.send_json(200, {"reading_interval_seconds": Device.interval})

    def __getattr__(self, name):
        # jede HTTP-Methode (GET, POST, PUT, TRACE, ...) landet in handle_request
        if name.startswith("do_"):
            return self.handle_request
        raise AttributeError(name)


def start(port=0):
    """Startet den Mock im Hintergrund und gibt den Server zurück."""
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"Mock-ESP32 auf http://127.0.0.1:{port} (Strg+C beendet)")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
