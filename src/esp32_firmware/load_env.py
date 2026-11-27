# Pre-Script für PlatformIO: erzeugt data/settings.toml aus der .env im Hauptverzeichnis.
#
# Alle Einstellungen (WLAN, MQTT, Gerät, Intervall, ...) werden nur in der .env gepflegt
# (Vorlage .env.example, .env steht in .gitignore). Bei jedem `pio run`, also auch bei
# `pio run -t uploadfs`, wird daraus data/settings.toml geschrieben, die die Firmware
# beim Start aus dem LittleFS liest. data/settings.toml ist generiert und ebenfalls ignoriert.
import os

Import("env")

project_dir = env.subst("$PROJECT_DIR")
env_file = os.path.normpath(os.path.join(project_dir, "..", "..", ".env"))
out_file = os.path.join(project_dir, "data", "settings.toml")

if not os.path.isfile(env_file):
    raise SystemExit(f"Datei {env_file} fehlt: im Hauptverzeichnis  cp .env.example .env  und ausfüllen.")

values = {}
with open(env_file, encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip('"').strip("'")


def need(key):
    value = values.get(key, "")
    if not value:
        raise SystemExit(f"In .env fehlt oder ist leer: {key}")
    if '"' in value or "\\" in value:
        raise SystemExit(f"{key} darf keine Anführungszeichen oder Backslashes enthalten.")
    return value


def need_int(key, minimum):
    value = need(key)
    if not value.isdigit() or int(value) < minimum:
        raise SystemExit(f"{key} muss eine Ganzzahl >= {minimum} sein, ist aber '{value}'.")
    return int(value)


group = need("MQTT_GROUP")
if group != group.lower() or "/" in group:
    raise SystemExit("MQTT_GROUP: nur Kleinbuchstaben, ohne '/', z.B. nachname1-nachname2")

settings = [
    ("device_id", need("DEVICE_ID")),
    ("wifi_ssid", need("WIFI_SSID")),
    ("wifi_password", need("WIFI_PASSWORD")),
    ("broker_address", need("MQTT_HOST")),
    ("broker_port", need_int("MQTT_PORT", 1)),
    ("mqtt_username", need("MQTT_USERNAME")),
    ("mqtt_password", need("MQTT_PASSWORD")),
    # Topic-Struktur aus docs/iot-specs/conventions.md
    ("telemetry_topic", f"iiot/group/{group}/sensor/temperature/state"),
    ("humidity_topic", f"iiot/group/{group}/sensor/humidity/state"),
    ("status_topic", f"iiot/group/{group}/status"),
    ("ntp_server", need("NTP_SERVER")),
    ("reading_interval_seconds", need_int("READING_INTERVAL_SECONDS", 2)),
    ("sensor_pin", need_int("SENSOR_PIN", 0)),
]

lines = ["# GENERIERT von load_env.py aus der .env im Hauptverzeichnis. Nicht von Hand ändern,",
         "# Änderungen in der .env vornehmen und  pio run -t uploadfs  ausführen."]
for key, value in settings:
    lines.append(f"{key} = {value}" if isinstance(value, int) else f'{key} = "{value}"')

os.makedirs(os.path.dirname(out_file), exist_ok=True)
content = "\n".join(lines) + "\n"
old = open(out_file, encoding="utf-8").read() if os.path.isfile(out_file) else None
if content != old:
    with open(out_file, "w", encoding="utf-8") as f:
        f.write(content)
    print("load_env.py: data/settings.toml aus .env erzeugt")
