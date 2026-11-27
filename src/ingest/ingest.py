"""Liest Messwerte und Status aller Gruppen vom MQTT-Broker und schreibt sie in InfluxDB.

Topics (siehe docs/iot-specs/conventions.md):
  iiot/group/<gruppe>/sensor/temperature/state  -> Measurement "temperature", Felder value (+ humidity_percent)
  iiot/group/<gruppe>/sensor/humidity/state     -> Measurement "temperature", Feld humidity_percent
  iiot/group/<gruppe>/status                     -> Measurement "status"
Das Humidity-Topic steht nicht in asyncapi.yaml, wird aber von mehreren Gruppen genutzt. Es landet im
selben Feld wie humidity_percent, damit das Dashboard die Luftfeuchte aller Gruppen gleich anzeigt.
Neue Gruppen erscheinen ohne Konfiguration, da alle Topics mit dem Platzhalter + abonniert werden.
Nachrichten, die nicht zur Konvention passen, werden protokolliert und verworfen.
"""

import json
import logging
import os
import sys
import time
import uuid
from datetime import datetime, timedelta, timezone

import paho.mqtt.client as mqtt
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS

TELEMETRY_TOPIC = "iiot/group/+/sensor/temperature/state"
HUMIDITY_TOPIC = "iiot/group/+/sensor/humidity/state"
STATUS_TOPIC = "iiot/group/+/status"
STATUS_VALUES = {"ok", "error", "offline"}

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("ingest")


def env(name, default=None):
    value = os.environ.get(name, default)
    if value in (None, ""):
        sys.exit(f"Umgebungsvariable {name} fehlt, siehe .env.example")
    return value


def parse_time(value, received):
    """Zeitstempel des Geräts (UTC, ISO 8601). Fehlt er oder ist er unplausibel: Empfangszeit."""
    try:
        timestamp = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
        if timestamp.tzinfo is None:
            raise ValueError
        if abs(timestamp - received) > timedelta(days=1):
            raise ValueError
        return timestamp
    except (TypeError, ValueError):
        return received


def number(value):
    """Zahl oder None. bool zählt nicht als Zahl."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return float(value)


def to_point(topic, payload, received):
    """Wandelt eine MQTT-Nachricht in einen InfluxDB-Punkt um. ValueError, wenn sie nicht passt."""
    parts = topic.split("/")
    if len(parts) < 4 or parts[:2] != ["iiot", "group"]:
        raise ValueError("unbekanntes Topic")
    group = parts[2]
    data = json.loads(payload)
    if not isinstance(data, dict):
        raise ValueError("kein JSON-Objekt")
    sensor_id = str(data.get("sensor_id") or group)

    if parts[3:] == ["sensor", "temperature", "state"]:
        value = number(data.get("value"))
        if value is None:
            raise ValueError("value fehlt oder ist keine Zahl")
        point = (
            Point("temperature")
            .tag("group", group)
            .tag("sensor_id", sensor_id)
            .field("value", value)
            .time(parse_time(data.get("timestamp"), received))
        )
        humidity = number(data.get("humidity_percent"))
        if humidity is not None:
            point.field("humidity_percent", humidity)
        return point

    if parts[3:] == ["sensor", "humidity", "state"]:
        value = number(data.get("value"))
        if value is None:
            raise ValueError("value fehlt oder ist keine Zahl")
        return (
            Point("temperature")
            .tag("group", group)
            .tag("sensor_id", sensor_id)
            .field("humidity_percent", value)
            .time(parse_time(data.get("timestamp"), received))
        )

    if parts[3:] == ["status"]:
        status = data.get("status")
        if status not in STATUS_VALUES:
            raise ValueError(f"status muss einer von {sorted(STATUS_VALUES)} sein")
        # Empfangszeit: retained Nachrichten kommen beim Start erneut, ihr Zeitstempel fehlt oft
        return Point("status").tag("group", group).field("status", status).field("sensor_id", sensor_id).time(received)

    raise ValueError("unbekanntes Topic")


def main():
    influx = InfluxDBClient(url=env("INFLUXDB_URL"), token=env("INFLUXDB_TOKEN"), org=env("INFLUXDB_ORG"))
    bucket = env("INFLUXDB_BUCKET")
    writer = influx.write_api(write_options=SYNCHRONOUS)
    while not influx.ping():
        log.info("warte auf InfluxDB ...")
        time.sleep(2)

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.error("MQTT-Verbindung abgelehnt: %s", reason_code)
            return
        log.info("mit MQTT-Broker verbunden, abonniere %s, %s und %s", TELEMETRY_TOPIC, HUMIDITY_TOPIC, STATUS_TOPIC)
        client.subscribe([(TELEMETRY_TOPIC, 0), (HUMIDITY_TOPIC, 0), (STATUS_TOPIC, 0)])

    def on_message(client, userdata, message):
        received = datetime.now(timezone.utc)
        try:
            point = to_point(message.topic, message.payload, received)
        except (ValueError, UnicodeDecodeError) as error:  # json.JSONDecodeError ist ein ValueError
            log.warning("verworfen %s: %s (%r)", message.topic, error, message.payload[:100])
            return
        try:
            writer.write(bucket=bucket, record=point)
        except Exception as error:  # InfluxDB kurz weg: Nachricht verwerfen, nicht abstürzen
            log.error("Schreiben in InfluxDB fehlgeschlagen: %s", error)

    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"{env('MQTT_CLIENT_ID_PREFIX', 'ingest')}-{uuid.uuid4().hex[:8]}",
        protocol=mqtt.MQTTv5,
    )
    client.username_pw_set(env("MQTT_USERNAME"), env("MQTT_PASSWORD"))
    client.on_connect = on_connect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    host, port = env("MQTT_HOST"), int(env("MQTT_PORT", "1883"))
    log.info("verbinde mit MQTT-Broker %s:%d", host, port)
    client.connect_async(host, port)
    client.loop_forever(retry_first_connection=True)


if __name__ == "__main__":
    main()
