# IIoT-Temperaturüberwachung – Gruppe nockenberg-pluder

Abgabe zur Lehrveranstaltung Industrial Internet of Things (MECH-M-DUAL-3-IOT, MCI, WS 2026).
Ein ESP32 misst mit einem DHT11 Temperatur und Luftfeuchte und sendet die Werte per MQTT an den Kurs-Broker. Ein Docker-Stack liest die Daten aller Gruppen vom Broker, speichert sie in InfluxDB und zeigt sie in Grafana an.

Aufgabenstellung und Kursunterlagen: <https://jhumci.github.io/MECH-M-3-IIoT/>

## Gruppe

| | |
|---|---|
| Mitglieder | Jens Nockenberg, Jonas Pluder |
| MQTT-Gruppenname | `nockenberg-pluder` |
| Telemetrie-Topic | `iiot/group/nockenberg-pluder/sensor/temperature/state` |
| Luftfeuchte-Topic | `iiot/group/nockenberg-pluder/sensor/humidity/state` (zusätzlich, siehe unten) |
| Status-Topic | `iiot/group/nockenberg-pluder/status` |

## Video

[Video](https://youtu.be/aRUb0O1-bOA) 

## Erreichter Stand

| Teil | Beanspruchte Stufe | Begründung |
|---|---|---|
| Termin 2 – Tests der HTTP-API | *sehr gut* | OpenAPI-Tooling: [Schemathesis](https://schemathesis.readthedocs.io/) erzeugt die Testfälle aus [`openapi.yaml`](docs/iot-specs/openapi.yaml), dazu `pytest`-Tests für das Verhalten ([`tests/`](tests/)) |
| Termin 3 – Cloud | *sehr gut* + Bonus | InfluxDB als Datenbank, alle Gruppen über `iiot/group/+/...` ohne Konfiguration, Geräteübersicht mit Anzahl, letzter Verbindung und Status, Kennzahlen Mittelwert/Minimum/Maximum |

**Was nicht oder nur eingeschränkt funktioniert:**

- Ein per `POST /config` gesetztes Sendeintervall gilt nur bis zum nächsten Neustart des ESP32. Danach gilt wieder `READING_INTERVAL_SECONDS` aus der `.env`.
- Der WebServer des ESP32 bedient nur eine Verbindung gleichzeitig, deshalb schicken die Tests `Connection: close`.
- Die Verbindung zum Broker ist unverschlüsselt (Port 1883), so wie der Kurs-Broker es vorgibt, siehe [Sicherheit](#sicherheit).

## Überblick

```
 ┌──────────────┐  MQTT (TCP 1883)   ┌──────────────────┐   MQTT    ┌───────────────── Docker ─────────────────┐
 │ ESP32+DHT11  │ ─────────────────► │  Kurs-Broker     │ ────────► │ ingest ──► InfluxDB ──► Grafana :3000    │
 │ HTTP-API :80 │  .../state, status │  158.180.44.197  │  alle     │ (Python)   (:8086)      (Dashboard)      │
 └──────────────┘                    └──────────────────┘  Gruppen  └──────────────────────────────────────────┘
        ▲ GET/POST /config, GET /status (nur im lokalen WLAN)
     Laptop / Tests
```

1. **Sensor:** Der ESP32 liest den DHT11 im eingestellten Intervall aus und sendet JSON mit UTC-Zeitstempel an `.../sensor/temperature/state` (Temperatur, dazu die Luftfeuchte als `humidity_percent`). Zusätzlich sendet er die Luftfeuchte im selben Schema auf `.../sensor/humidity/state`. Dieses Topic steht nicht in `asyncapi.yaml`, wird aber von mehreren Gruppen so genutzt. Auf `.../status` meldet er retained `ok` oder `error`. Fällt er weg, sendet der Broker seinen Last Will `{"status": "offline"}`.
2. **Broker:** Der vorgegebene Kurs-Broker, er gehört nicht zu dieser Lösung.
3. **Speicher:** Der Dienst `ingest` abonniert `iiot/group/+/sensor/temperature/state`, `iiot/group/+/sensor/humidity/state` und `iiot/group/+/status`, prüft die Nachrichten und schreibt sie in InfluxDB. Nachrichten, die nicht zur Konvention passen, protokolliert er und verwirft sie, ohne abzustürzen.
4. **Dashboard:** Grafana liest aus InfluxDB. Datenquelle und Dashboard werden beim Start automatisch eingerichtet.

### Struktur des Repositories

| Pfad | Inhalt |
|---|---|
| [`.env.example`](.env.example) | Vorlage für **alle** Einstellungen (ESP32 und Container), wird nach `.env` kopiert |
| [`docker-compose.yml`](docker-compose.yml) | Container-Stack: InfluxDB, ingest, Grafana |
| [`src/esp32_firmware/`](src/esp32_firmware/) | Firmware (PlatformIO/Arduino): [`src/main.cpp`](src/esp32_firmware/src/main.cpp), [`load_env.py`](src/esp32_firmware/load_env.py) erzeugt `data/settings.toml` aus der `.env` |
| [`src/ingest/`](src/ingest/) | MQTT → InfluxDB ([`ingest.py`](src/ingest/ingest.py)) |
| [`src/Dashboard/`](src/Dashboard/) | Grafana-Provisioning und Dashboard ([`iiot.json`](src/Dashboard/dashboards/iiot.json)) |
| [`tests/`](tests/) | Tests der HTTP-API (Schemathesis + pytest) und ein Mock des Geräts |
| [`docs/iot-specs/`](docs/iot-specs/) | Verbindliche Spezifikationen: [`conventions.md`](docs/iot-specs/conventions.md), [`asyncapi.yaml`](docs/iot-specs/asyncapi.yaml), [`openapi.yaml`](docs/iot-specs/openapi.yaml) |
| `docs/`, `index.qmd` | Kursunterlagen (Quarto) |

## Einstellungen: eine `.env` für alles

Alle Einstellungen und Passwörter stehen in **einer** Datei `.env` im Hauptverzeichnis. Sie steht in `.gitignore` und wird nie committet. Die Vorlage ist [`.env.example`](.env.example):

```bash
git clone https://github.com/J0n1lu5/MECH-M-3-IIoT.git
cd MECH-M-3-IIoT
cp .env.example .env
```

Danach `.env` in einem Editor öffnen und ausfüllen:

| Variable | Bedeutung |
|---|---|
| `WIFI_SSID`, `WIFI_PASSWORD` | WLAN vor Ort, **nur 2,4 GHz** (der ESP32 kann kein 5 GHz) |
| `DEVICE_ID` | Eindeutiger Name des Sensors, z.B. `nockenberg-pluder-esp32-01` |
| `MQTT_GROUP` | Gruppenname für die Topics, nur Kleinbuchstaben: `nockenberg-pluder` |
| `READING_INTERVAL_SECONDS` | Sendeintervall in Sekunden, mindestens 2 |
| `SENSOR_PIN` | GPIO der DHT11-Datenleitung, Standard `4` |
| `NTP_SERVER` | Zeitserver für die UTC-Zeitstempel, Standard `pool.ntp.org` |
| `MQTT_HOST`, `MQTT_PORT`, `MQTT_USERNAME` | Kurs-Broker, vorausgefüllt |
| `MQTT_PASSWORD` | Passwort des Kurs-Brokers (in der Vorlesung erfragen) |
| `INFLUXDB_ADMIN_PASSWORD` | frei wählbar, mindestens 8 Zeichen |
| `INFLUXDB_ADMIN_TOKEN` | lange Zufallszeichenkette, z.B. aus `openssl rand -hex 32` |
| `GRAFANA_ADMIN_PASSWORD` | frei wählbar, Login für das Dashboard |

Die Kursvorgabe einer `settings.toml` ist erfüllt: `load_env.py` erzeugt beim Build aus der `.env` die Datei `src/esp32_firmware/data/settings.toml`, die die Firmware beim Start aus ihrem Dateisystem liest. Diese Datei ist generiert und ebenfalls in `.gitignore`. Man ändert sie nicht von Hand.

## Mikrocontroller einrichten

### Hardware und Verkabelung

- ESP32-DevKit (z.B. ESP32-WROOM-32), USB-Datenkabel (kein reines Ladekabel)
- DHT11 und 10 kΩ-Widerstand (entfällt bei DHT11-Modulen mit eingebautem Pull-up), Steckbrett, Jumper-Kabel

| ESP32 | | DHT11 |
|---|---|---|
| 3V3 | → | Pin 1 – VCC |
| GPIO 4 (`SENSOR_PIN`) | → | Pin 2 – DATA |
| – | | Pin 3 – nicht verbunden |
| GND | → | Pin 4 – GND |

Der 10 kΩ-Widerstand kommt zwischen DATA und 3V3. Den DHT11 ein paar Zentimeter vom ESP32 entfernt platzieren, sonst misst er dessen Abwärme mit. Bild und Pinout: [`docs/Materialien.qmd`](docs/Materialien.qmd).

### Firmware aufspielen

Voraussetzung ist [PlatformIO](https://platformio.org/install/cli), entweder die PlatformIO-Erweiterung in VS Code oder die Kommandozeile `pio`. Unter Linux muss der Benutzer Zugriff auf den seriellen Port haben (Gruppe `uucp` bzw. `dialout`).

1. `.env` wie oben ausfüllen.
2. ESP32 per USB anschließen.
3. Firmware und Einstellungen aufspielen und die serielle Konsole öffnen:

```bash
cd src/esp32_firmware
pio run -t upload        # Firmware flashen
pio run -t uploadfs      # Einstellungen aus der .env als settings.toml aufspielen
pio device monitor       # serielle Konsole, 115200 Baud, beenden mit Strg+C
```

Bleibt der Upload bei `Connecting....` hängen, während des Verbindens die **BOOT**-Taste gedrückt halten. Fehlt ein Wert in der `.env`, bricht der Build mit einer Meldung ab, welche Variable fehlt.

### In ein anderes WLAN einbinden

Nur `WIFI_SSID` und `WIFI_PASSWORD` in der `.env` ändern und `pio run -t uploadfs` ausführen. Die Firmware muss dafür nicht neu geflasht werden. Dasselbe gilt für Intervall, Pin, Gerätename und Broker.

### Prüfen, ob es funktioniert

1. **Serielle Konsole:** Nach dem Start erscheinen `WLAN verbinden mit ...`, die IP-Adresse, `HTTP-API: http://<ip>/status`, `MQTT verbinden... ok` und `Gesendet: {...}` im eingestellten Intervall.
   - `MQTT ... fehlgeschlagen (rc=5)`: Benutzername oder Passwort falsch.
   - `DHT11 liefert keinen Wert`: Verkabelung bzw. `SENSOR_PIN` prüfen. Der Status wechselt dann auf `error`.
   - `settings.toml fehlt`: `pio run -t uploadfs` wurde noch nicht ausgeführt.
2. **MQTT:** Im [MQTT Explorer](https://mqtt-explorer.com/) oder mit `mqttui` mit dem Kurs-Broker verbinden. Unter `iiot/group/nockenberg-pluder/` erscheinen `sensor/temperature/state`, `sensor/humidity/state` und `status`. Wird der ESP32 vom Strom getrennt, steht nach ca. 20–30 s (MQTT-Keepalive 15 s × 1,5) `{"status":"offline", ...}` im Status-Topic.
3. **HTTP:** `curl http://<ip>/status`, siehe [HTTP-API](#http-api).

Beispiel einer Messung (beide Nachrichten mit demselben Zeitstempel):

```json
// iiot/group/nockenberg-pluder/sensor/temperature/state
{"timestamp":"2026-11-27T00:29:42Z","sensor_id":"nockenberg-pluder-esp32-01","value":21.4,"unit":"°C","humidity_percent":51}
// iiot/group/nockenberg-pluder/sensor/humidity/state
{"timestamp":"2026-11-27T00:29:42Z","sensor_id":"nockenberg-pluder-esp32-01","value":51,"unit":"%"}
```

Mit `mqttui` (im Hauptverzeichnis, Shell fish):

```fish
set -x MQTTUI_PASSWORD (string replace 'MQTT_PASSWORD=' '' (grep '^MQTT_PASSWORD=' .env))
mqttui -b mqtt://158.180.44.197:1883 -u bobm 'iiot/group/nockenberg-pluder/#'
```

Messwerte sind nicht retained und erscheinen erst mit der nächsten Messung, der Status sofort.

## Container starten

**Voraussetzungen:** Docker Engine mit dem Compose-Plugin (`docker compose version` muss funktionieren), Zugang ins Internet zum Broker auf TCP 1883.

```bash
git clone https://github.com/J0n1lu5/MECH-M-3-IIoT.git
cd MECH-M-3-IIoT
cp .env.example .env        # MQTT_PASSWORD, INFLUXDB_ADMIN_PASSWORD, INFLUXDB_ADMIN_TOKEN, GRAFANA_ADMIN_PASSWORD ausfüllen
docker compose up -d --build
docker compose ps           # influxdb (healthy), ingest und grafana laufen
docker compose logs -f ingest   # "mit MQTT-Broker verbunden, abonniere ..."
```

Für die Container werden die `WIFI_*`- und Geräte-Variablen nicht gebraucht, sie dürfen leer bleiben.

Beenden mit `docker compose down`. Die Daten bleiben in Docker-Volumes erhalten. `docker compose down -v` löscht sie. Achtung: InfluxDB übernimmt Benutzer, Passwort und Token aus der `.env` nur beim **ersten** Start. Wer sie später ändert, muss die Volumes mit `docker compose down -v` löschen.

| Dienst | Aufgabe | Port auf dem Host |
|---|---|---|
| `influxdb` | Zeitreihendatenbank (InfluxDB 2) | `127.0.0.1:8086`, nur lokal |
| `ingest` | MQTT → InfluxDB, Python, eigenes Image aus [`src/ingest/`](src/ingest/) | – |
| `grafana` | Dashboard | `3000` |

## Dashboard nutzen

- **URL:** <http://localhost:3000>, vom Handy oder einem anderen Rechner im selben Netz `http://<IP-des-Docker-Hosts>:3000`
- **Login:** Benutzer `admin`, Passwort `GRAFANA_ADMIN_PASSWORD` aus der `.env`
- Das Dashboard **IIoT Temperatur** ist die Startseite. Es aktualisiert sich alle 10 s und zeigt standardmäßig die letzten 6 Stunden, oben rechts einstellbar. Über die Auswahl **group** oben links lassen sich Zeitreihen und Kennzahlen auf einzelne Gruppen einschränken.

| Ansicht | Inhalt |
|---|---|
| Sensoren gesamt | Anzahl der Gruppen, von denen in den letzten 30 Tagen eine Messung oder ein Status ankam |
| Sensoren online | Gruppen, deren letzter Status `ok` oder `error` ist (nicht `offline`) |
| Temperatur / Luftfeuchte / Status nockenberg-pluder | Aktuelle Werte des eigenen Sensors |
| Temperatur, Luftfeuchte | Zeitreihen aller Gruppen, eine Linie pro Gruppe. Die Luftfeuchte kommt aus `humidity_percent` oder aus `.../sensor/humidity/state`, je nachdem, was eine Gruppe sendet |
| Geräteübersicht | Alle Gruppen: letzter Status aus `iiot/group/+/status`, Zeitpunkt der letzten Verbindung, letzte Messung |
| Kennzahlen im gewählten Zeitraum | Mittelwert, Minimum und Maximum je Gruppe |

**Blick in die Datenbank:** InfluxDB-Oberfläche unter <http://localhost:8086>, Login `INFLUXDB_ADMIN_USER` / `INFLUXDB_ADMIN_PASSWORD`, unter *Data Explorer* den Bucket `iiot` mit den Measurements `temperature` und `status` öffnen.

**Neuer Sensor ohne Konfiguration:** Eine Testnachricht erscheint nach wenigen Sekunden als neue Gruppe im Dashboard:

```bash
set -a; . ./.env; set +a     # MQTT_PASSWORD usw. aus der .env laden
mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" -u "$MQTT_USERNAME" -P "$MQTT_PASSWORD" \
  -t iiot/group/test-pluder/sensor/temperature/state \
  -m "{\"timestamp\":\"$(date -u +%Y-%m-%dT%H:%M:%SZ)\",\"sensor_id\":\"test-pluder\",\"value\":21.5,\"unit\":\"°C\"}"
```

## HTTP-API

Die Firmware setzt [`openapi.yaml`](docs/iot-specs/openapi.yaml) (Version 1.2.0) um. Basis-URL ist `http://<ip>/` auf Port 80, unverschlüsselt und nur im lokalen WLAN erreichbar. Die IP zeigt die serielle Konsole.

| Methode | Pfad | Funktion |
|---|---|---|
| `GET` | `/config` | `device_id` und `reading_interval_seconds` |
| `POST` | `/config` | Sendeintervall ändern (Ganzzahl ≥ 2). `device_id` wird ignoriert. Fehlerhafte Eingaben → `400` |
| `GET` | `/status` | Laufzeit, WLAN- und MQTT-Verbindung, letzte Messung. Zusätzlich `status` und `ip` |

Andere Methoden ergeben `405`, unbekannte Pfade `404`, jeweils mit `{"error": "..."}`.

```bash
DEVICE=http://192.168.0.42          # IP aus der seriellen Konsole

curl -s $DEVICE/status
# {"device_id":"nockenberg-pluder-esp32-01","uptime_seconds":150,"wifi_connected":true,"mqtt_connected":true,
#  "status":"ok","ip":"192.168.0.42","last_telemetry":{"timestamp":"2026-11-26T14:17:02Z","value":22.8,"unit":"°C"},
#  "last_humidity_percent":61}

curl -s $DEVICE/config
# {"device_id":"nockenberg-pluder-esp32-01","reading_interval_seconds":60}

curl -s -X POST $DEVICE/config -H "Content-Type: application/json" -d '{"reading_interval_seconds": 10}'
# {"reading_interval_seconds":10}
```

Das neue Intervall gilt sofort und ist an den Zeitstempeln der MQTT-Nachrichten zu sehen. Nach einem Neustart gilt wieder der Wert aus der `.env`.

### Testfälle ausführen

| Datei | Werkzeug | Prüft |
|---|---|---|
| [`tests/test_openapi_schemathesis.py`](tests/test_openapi_schemathesis.py) | Schemathesis | Erzeugt aus `openapi.yaml` gültige und ungültige Anfragen und prüft Statuscodes, Content-Type und Antwortschema |
| [`tests/test_http_api.py`](tests/test_http_api.py) | pytest + requests | `POST /config` ändert das Intervall, `device_id` bleibt gleich, Messwerte plausibel, neues Intervall erzeugt neue Messung |
| [`tests/mock_device.py`](tests/mock_device.py) | Standardbibliothek | Nachbildung der Geräte-API, damit die Tests auch ohne Board laufen |

Im Hauptverzeichnis, Python ≥ 3.10:

```bash
python3 -m venv .venv
source .venv/bin/activate                      # Windows: .venv\Scripts\Activate.ps1
pip install -r tests/requirements.txt

DEVICE_URL=http://192.168.0.42 pytest tests -v # gegen das echte Gerät (Laptop im selben WLAN)
pytest tests -v                                # ohne DEVICE_URL: gegen den Mock
```

Erwartet: `5 passed`. Der Testlauf ändert das Intervall und stellt am Ende den Ausgangswert wieder her. Details und Schemathesis über die Kommandozeile: [`tests/README.md`](tests/README.md).

## Hinweise für die IT-Abteilung

Das WLAN vor Ort ist durch eine Firewall mit Whitelist geschützt. Damit die Messwerte beim Broker ankommen, braucht der ESP32 folgende Freigaben:

**Ausgehend (vom ESP32):**

| Ziel | Port / Protokoll | Zweck |
|---|---|---|
| `158.180.44.197` | TCP 1883 | MQTT zum Kurs-Broker (unverschlüsselt) |
| `pool.ntp.org` (wechselnde IPs) bzw. der eingestellte `NTP_SERVER` | UDP 123 | Uhrzeit für die UTC-Zeitstempel. Ohne NTP sendet das Gerät **keine** Messwerte |
| DNS-Server aus DHCP | UDP/TCP 53 | Namensauflösung des NTP-Servers |
| DHCP-Server | UDP 67/68 | IP-Adresse beziehen |

Da sich die IPs von `pool.ntp.org` ändern, ist es einfacher, einen festen internen NTP-Server in `NTP_SERVER` einzutragen und nur diesen freizugeben.

**Eingehend (zum ESP32):** TCP 80 (HTTP-API), **nur aus dem lokalen Netz**, nicht aus dem Internet. Empfehlenswert ist eine DHCP-Reservierung, damit der ESP32 immer dieselbe IP bekommt. Client-Isolation im WLAN muss für den Wartungs-Laptop ausgeschaltet sein, sonst ist die HTTP-API nicht erreichbar.

**Docker-Host:** ausgehend TCP 1883 zum Broker, eingehend TCP 3000 für das Dashboard. InfluxDB (8086) ist nur auf `127.0.0.1` gebunden.

**Fernzugriff auf die HTTP-API:** Die API ist absichtlich nur lokal erreichbar. Mögliche Wege für die Wartung aus Innsbruck:

- **VPN (empfohlen):** Ein VPN-Zugang (z.B. WireGuard oder das Firmen-VPN) ins Standortnetz. Die API bleibt unverändert, die Verbindung ist verschlüsselt und authentifiziert. Hindernis: Die IT jedes Standorts muss das VPN einrichten und freigeben.
- **Konfiguration über MQTT:** Das Gerät abonniert ein Konfigurations-Topic. Diese Verbindung baut das Gerät selbst nach außen auf, deshalb braucht es keine eingehende Freigabe. Das ist für weltweit verteilte Geräte meist der beste Weg, wäre aber eine Erweiterung der Firmware.
- **Portweiterleitung / öffentliche IP: nicht empfohlen.** Die API hat keine Authentifizierung und kein TLS. Jeder im Internet könnte das Intervall ändern. Der einfache WebServer des ESP32 bedient nur eine Verbindung gleichzeitig und ist leicht lahmzulegen. Außerdem verhindern NAT, wechselnde IPs und Firmen-Firewalls das meist ohnehin.
- **Reverse-Proxy oder Tunnel** (z.B. ein Raspberry Pi vor Ort mit TLS und Login) wäre ein Kompromiss, braucht aber zusätzliche Hardware und Wartung.

## Sicherheit

Diese Lösung ist ein Prototyp. Eine Produktivversion bräuchte zusätzlich: TLS für MQTT (Port 8883) mit eigenen Zugangsdaten pro Gerät statt eines gemeinsamen Kurs-Kontos, ACLs am Broker, damit jedes Gerät nur auf sein eigenes Topic schreiben darf, Authentifizierung und HTTPS für die HTTP-API oder deren Ersatz durch Konfiguration über MQTT, signierte Firmware-Updates über das Netz (OTA), Grafana hinter einem Reverse-Proxy mit HTTPS statt offenem Port 3000, Backups der InfluxDB-Volumes, ein Monitoring des `ingest`-Dienstes und eine sichere Verwaltung der Zugangsdaten (z.B. Docker Secrets statt `.env`). Der Flash des ESP32 ist nicht verschlüsselt. Wer das Gerät in die Hände bekommt, kann WLAN- und MQTT-Passwort auslesen.
