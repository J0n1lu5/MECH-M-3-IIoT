// ESP32 + DHT11: Messwerte per MQTT senden, Konfiguration per HTTP (siehe docs/iot-specs).
// Alle Einstellungen stehen in data/settings.toml (Vorlage: ../settings.toml), siehe README.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <map>

std::map<String, String> settings;  // Inhalt von settings.toml
String deviceId, telemetryTopic, statusTopic, lastWill, brokerAddress;  // global, da PubSubClient nur Zeiger speichert
unsigned long intervalSeconds = 60;

DHT* dht;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer server(80);

unsigned long lastReading = 0;
unsigned long lastMqttAttempt = 0;
String status = "ok";             // "ok" oder "error", wird retained auf statusTopic gesendet
String lastTimestamp;             // leer, solange noch kein Messwert gesendet wurde
float lastTemperature = NAN;
float lastHumidity = NAN;

// ---------- settings.toml ----------

// Liest Zeilen der Form  schluessel = "text"  oder  schluessel = 123  (Kommentare mit #).
bool loadSettings() {
    if (!LittleFS.begin()) return false;
    File file = LittleFS.open("/settings.toml", "r");
    if (!file) return false;
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (line.startsWith("#") || eq < 0) continue;
        String key = line.substring(0, eq);
        String value = line.substring(eq + 1);
        key.trim();
        value.trim();
        if (value.startsWith("\"")) {
            value = value.substring(1, value.indexOf('"', 1));
        } else if (value.indexOf('#') >= 0) {
            value = value.substring(0, value.indexOf('#'));
            value.trim();
        }
        settings[key] = value;
    }
    file.close();
    return true;
}

String setting(const char* key, const char* fallback = "") {
    return settings.count(key) ? settings[key] : String(fallback);
}

// ---------- Zeit (UTC, ISO8601) ----------

String utcTimestamp() {
    time_t now = time(nullptr);
    if (now < 1700000000) return "";  // noch keine Zeit vom NTP-Server
    char buffer[21];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    return buffer;
}

// ---------- MQTT ----------

void publishStatus() {
    JsonDocument doc;
    doc["status"] = status;
    doc["sensor_id"] = deviceId;
    String payload;
    serializeJson(doc, payload);
    mqtt.publish(statusTopic.c_str(), payload.c_str(), true);
    Serial.println("Status: " + payload);
}

void setStatus(const String& newStatus) {
    if (newStatus == status) return;
    status = newStatus;
    if (mqtt.connected()) publishStatus();
}

// Versucht höchstens alle 5 s eine Verbindung, damit HTTP und Sensor weiterlaufen.
void keepMqttConnected() {
    if (mqtt.connected()) {
        mqtt.loop();
        return;
    }
    if (WiFi.status() != WL_CONNECTED || millis() - lastMqttAttempt < 5000) return;
    lastMqttAttempt = millis();
    Serial.print("MQTT verbinden... ");
    // Last Will: Der Broker sendet {"status":"offline"} retained, wenn das Gerät wegfällt.
    if (mqtt.connect(deviceId.c_str(), setting("mqtt_username").c_str(), setting("mqtt_password").c_str(),
                     statusTopic.c_str(), 1, true, lastWill.c_str())) {
        Serial.println("ok");
        publishStatus();
    } else {
        Serial.printf("fehlgeschlagen (rc=%d)\n", mqtt.state());
    }
}

// ---------- Sensor ----------

void measureAndPublish() {
    float temperature = dht->readTemperature();
    float humidity = dht->readHumidity();
    if (isnan(temperature) || isnan(humidity)) {
        Serial.println("DHT11 liefert keinen Wert");
        setStatus("error");
        return;
    }
    setStatus("ok");
    lastHumidity = humidity;

    String timestamp = utcTimestamp();
    if (timestamp.isEmpty() || !mqtt.connected()) {
        Serial.printf("%.1f °C gemessen, aber noch keine NTP-Zeit oder MQTT-Verbindung\n", temperature);
        return;
    }
    JsonDocument doc;
    doc["timestamp"] = timestamp;
    doc["sensor_id"] = deviceId;
    doc["value"] = temperature;
    doc["unit"] = "°C";
    doc["humidity_percent"] = humidity;
    String payload;
    serializeJson(doc, payload);
    if (mqtt.publish(telemetryTopic.c_str(), payload.c_str())) {
        lastTimestamp = timestamp;
        lastTemperature = temperature;
        Serial.println("Gesendet: " + payload);
    }
}

// ---------- HTTP-API (openapi.yaml) ----------

void sendJson(int code, JsonDocument& doc) {
    String body;
    serializeJson(doc, body);
    server.send(code, "application/json", body);
}

void sendError(int code, const String& message) {
    JsonDocument doc;
    doc["error"] = message;
    sendJson(code, doc);
}

void handleConfig() {
    JsonDocument doc;
    if (server.method() == HTTP_GET) {
        doc["device_id"] = deviceId;
        doc["reading_interval_seconds"] = intervalSeconds;
        sendJson(200, doc);
    } else if (server.method() == HTTP_POST) {
        JsonDocument body;
        if (deserializeJson(body, server.arg("plain")) || !body.is<JsonObject>()) {
            return sendError(400, "Fehlerhaftes JSON");
        }
        JsonVariant interval = body["reading_interval_seconds"];
        if (!interval.is<long>() || interval.as<long>() < 2) {
            return sendError(400, "reading_interval_seconds muss eine Ganzzahl >= 2 sein");
        }
        // device_id wird ignoriert: Sie darf per POST nicht geändert werden.
        intervalSeconds = interval.as<long>();
        Serial.printf("Neues Intervall: %lu s\n", intervalSeconds);
        doc["reading_interval_seconds"] = intervalSeconds;
        sendJson(200, doc);
    } else {
        sendError(405, "Nur GET und POST erlaubt");
    }
}

void handleStatus() {
    if (server.method() != HTTP_GET) return sendError(405, "Nur GET erlaubt");
    JsonDocument doc;
    doc["device_id"] = deviceId;
    doc["uptime_seconds"] = millis() / 1000;
    doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
    doc["mqtt_connected"] = mqtt.connected();
    doc["status"] = status;                     // zusätzlich: wie im MQTT-Status-Topic
    doc["ip"] = WiFi.localIP().toString();      // zusätzlich: hilft bei der Fehlersuche
    if (lastTimestamp.isEmpty()) {
        doc["last_telemetry"] = nullptr;
    } else {
        doc["last_telemetry"]["timestamp"] = lastTimestamp;
        doc["last_telemetry"]["value"] = lastTemperature;
        doc["last_telemetry"]["unit"] = "°C";
    }
    if (isnan(lastHumidity)) {
        doc["last_humidity_percent"] = nullptr;
    } else {
        doc["last_humidity_percent"] = lastHumidity;
    }
    sendJson(200, doc);
}

// ---------- Start und Hauptschleife ----------

void setup() {
    Serial.begin(115200);
    delay(500);

    if (!loadSettings()) {
        while (true) {
            Serial.println("settings.toml fehlt: data/settings.toml anlegen und 'pio run -t uploadfs' ausfuehren");
            delay(5000);
        }
    }
    deviceId = setting("device_id");
    telemetryTopic = setting("telemetry_topic");
    statusTopic = setting("status_topic");
    intervalSeconds = max(2L, setting("reading_interval_seconds", "60").toInt());
    lastWill = "{\"status\":\"offline\",\"sensor_id\":\"" + deviceId + "\"}";

    dht = new DHT(setting("sensor_pin", "4").toInt(), DHT11);
    dht->begin();

    WiFi.begin(setting("wifi_ssid").c_str(), setting("wifi_password").c_str());
    Serial.print("WLAN verbinden");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nIP: " + WiFi.localIP().toString());

    configTime(0, 0, setting("ntp_server", "pool.ntp.org").c_str());  // UTC
    brokerAddress = setting("broker_address");
    mqtt.setServer(brokerAddress.c_str(), setting("broker_port", "1883").toInt());

    server.on("/config", handleConfig);
    server.on("/status", handleStatus);
    server.onNotFound([]() { sendError(404, "Unbekannter Pfad"); });
    server.begin();
    Serial.println("HTTP-API: http://" + WiFi.localIP().toString() + "/status");
}

void loop() {
    keepMqttConnected();
    server.handleClient();
    if (lastReading == 0 || millis() - lastReading >= intervalSeconds * 1000) {
        lastReading = millis();
        measureAndPublish();
    }
}
