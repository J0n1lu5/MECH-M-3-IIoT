# Testfälle für die HTTP-API

Die Tests prüfen die HTTP-API des ESP32 gegen [`docs/iot-specs/openapi.yaml`](../docs/iot-specs/openapi.yaml) (Version 1.2.0) auf zwei Wegen:

| Datei | Werkzeug | Was geprüft wird |
|---|---|---|
| `test_openapi_schemathesis.py` | Schemathesis (OpenAPI-Tooling) | Erzeugt aus `openapi.yaml` automatisch gültige und ungültige Anfragen (falsche Typen, fehlende Felder, falsche Methoden) und prüft Statuscode, Content-Type und Antwortschema |
| `test_http_api.py` | pytest + requests | Was die Spezifikation allein nicht prüfen kann: `POST /config` ändert das Intervall, `device_id` bleibt unverändert, Messwerte sind plausibel, ein neues Intervall erzeugt eine neue Messung |
| `mock_device.py` | Python-Standardbibliothek | Bildet die API der Firmware nach, damit die Tests auch ohne Board laufen |

Ein Testlauf ändert das Sendeintervall. Am Ende stellt er den Ausgangswert wieder her.

## Einrichten (einmalig)

Voraussetzung ist Python 3.10 oder neuer. Alle Befehle im Hauptverzeichnis des Repositories ausführen.

```bash
python3 -m venv .venv
source .venv/bin/activate          # Windows PowerShell: .venv\Scripts\Activate.ps1
pip install -r tests/requirements.txt
```

## Gegen das echte Gerät

Laptop und ESP32 müssen im selben WLAN sein. Die IP-Adresse zeigt die serielle Konsole nach dem Start (`HTTP-API: http://<ip>/status`).

```bash
DEVICE_URL=http://192.168.1.42 pytest tests -v
```

Unter Windows PowerShell:

```powershell
$env:DEVICE_URL = "http://192.168.1.42"; pytest tests -v
```

Erwartet: `5 passed`. Ohne MQTT-Verbindung wird `test_new_interval_produces_new_measurement` übersprungen, solange noch keine Messung gesendet wurde auch `test_last_measurement_is_plausible`.

## Ohne Gerät (Mock)

Ohne `DEVICE_URL` starten die Tests den Mock automatisch:

```bash
pytest tests -v
```

Für Versuche mit `curl` lässt sich der Mock auch von Hand starten:

```bash
python tests/mock_device.py 8080
curl -i http://127.0.0.1:8080/status
```

## Schemathesis über die Kommandozeile

Dieselbe Prüfung wie `test_openapi_schemathesis.py`, mit einem ausführlicheren Bericht:

```bash
schemathesis run docs/iot-specs/openapi.yaml --url http://192.168.1.42 -H "Connection: close" --max-examples 25
```

`Connection: close` ist nötig, weil der WebServer des ESP32 nur eine Verbindung gleichzeitig bedient.
