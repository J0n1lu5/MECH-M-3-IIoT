// ESP32 + DHT11: Messwerte per MQTT senden, Konfiguration per HTTP (siehe docs/iot-specs).
// Alle Einstellungen stehen in der .env im Hauptverzeichnis. load_env.py erzeugt daraus beim
// Build data/settings.toml, die per `pio run -t uploadfs` ins LittleFS kommt, siehe README.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <map>

std::map<String, String> settings;  // Inhalt von settings.toml
String deviceId, telemetryTopic, humidityTopic, statusTopic, lastWill, brokerAddress;  // global, da PubSubClient nur Zeiger speichert
String ntpServer;  // global, da configTime/SNTP nur den Zeiger auf den Namen speichert
unsigned long intervalSeconds = 60;
const unsigned long MAX_INTERVAL_SECONDS = 4294967;  // ULONG_MAX / 1000, wie in tests/mock_device.py

DHT* dht;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WiFiServer httpServer(80);

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

    // Zusätzlich die Luftfeuchte auf einem eigenen Topic, wie es andere Gruppen machen
    // (nicht in asyncapi.yaml vorgeschrieben, aber dasselbe Schema wie die Temperatur).
    JsonDocument humidityDoc;
    humidityDoc["timestamp"] = timestamp;
    humidityDoc["sensor_id"] = deviceId;
    humidityDoc["value"] = humidity;
    humidityDoc["unit"] = "%";
    String humidityPayload;
    serializeJson(humidityDoc, humidityPayload);
    if (mqtt.publish(humidityTopic.c_str(), humidityPayload.c_str())) {
        Serial.println("Gesendet: " + humidityPayload);
    }
}

// ---------- HTTP-API (openapi.yaml) ----------
// Eigener, minimaler HTTP-Server auf WiFiServer statt der WebServer-Bibliothek: Diese schließt bei
// unbekannten Methoden (z.B. QUERY) die Verbindung ohne Antwort und hängt bei multipart/form-data.
// Verhalten wie tests/mock_device.py: jede Methode wird beantwortet, der Body wird immer als JSON gelesen.

const unsigned long HTTP_TIMEOUT_MS = 2000;  // länger wartet das Gerät nicht auf eine Anfrage
const long MAX_BODY_BYTES = 1024;
const unsigned int MAX_LINE_LENGTH = 512;

String toJson(JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    return out;
}

String jsonError(const String& message) {
    JsonDocument doc;
    doc["error"] = message;
    return toJson(doc);
}

const char* reasonPhrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        default: return "Error";
    }
}

void sendResponse(WiFiClient& client, const String& method, int code, const String& body, const char* allow = nullptr) {
    String head = "HTTP/1.1 " + String(code) + " " + reasonPhrase(code) + "\r\n";
    head += "Content-Type: application/json\r\n";
    head += "Content-Length: " + String(body.length()) + "\r\n";
    if (allow) head += "Allow: " + String(allow) + "\r\n";
    head += "Connection: close\r\n\r\n";
    client.print(head);
    if (method != "HEAD") client.print(body);
}

bool timeLeft(unsigned long deadline) { return (long)(deadline - millis()) > 0; }

// Liest eine Zeile bis '\n' ohne Zeilenende. false bei Timeout, Verbindungsende oder zu langer Zeile.
bool readLine(WiFiClient& client, String& line, unsigned long deadline) {
    line = "";
    while (timeLeft(deadline)) {
        if (!client.available()) {
            if (!client.connected()) return false;
            delay(1);
            continue;
        }
        char c = client.read();
        if (c == '\n') {
            line.trim();
            return true;
        }
        if (line.length() >= MAX_LINE_LENGTH) return false;
        line += c;
    }
    return false;
}

String configJson() {
    JsonDocument doc;
    doc["device_id"] = deviceId;
    doc["reading_interval_seconds"] = intervalSeconds;
    return toJson(doc);
}

int postConfig(const String& body, String& out) {
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc.is<JsonObject>()) {
        out = jsonError("Fehlerhaftes JSON");
        return 400;
    }
    // Jede ganze JSON-Zahl >= 2, auch 60.0 oder Werte über 2^63 (die ArduinoJson als Gleitkommazahl
    // speichert), aber keine bool-Werte. Große Werte werden begrenzt, damit intervalSeconds * 1000
    // (Millisekunden) nicht überläuft.
    JsonVariant interval = doc["reading_interval_seconds"];
    double value = interval.as<double>();
    bool isNumber = !interval.is<bool>() && (interval.is<long long>() || interval.is<double>());
    if (!isNumber || !isfinite(value) || value < 2 || value != floor(value)) {
        out = jsonError("reading_interval_seconds muss eine Ganzzahl >= 2 sein");
        return 400;
    }
    // device_id wird ignoriert: Sie darf per POST nicht geändert werden.
    intervalSeconds = value > MAX_INTERVAL_SECONDS ? MAX_INTERVAL_SECONDS : (unsigned long)value;
    Serial.printf("Neues Intervall: %lu s\n", intervalSeconds);
    JsonDocument answer;
    answer["reading_interval_seconds"] = intervalSeconds;
    out = toJson(answer);
    return 200;
}

String statusJson() {
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
    return toJson(doc);
}

// Beantwortet höchstens eine wartende Anfrage pro Aufruf.
void handleHttp() {
    WiFiClient client = httpServer.available();
    if (!client) return;
    unsigned long deadline = millis() + HTTP_TIMEOUT_MS;

    // Anfragezeile, z.B. "POST /config HTTP/1.1"
    String requestLine;
    if (!readLine(client, requestLine, deadline)) {
        client.stop();
        return;
    }
    int firstSpace = requestLine.indexOf(' ');
    int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
    if (firstSpace <= 0 || secondSpace < 0) {
        sendResponse(client, "", 400, jsonError("Fehlerhafte Anfrage"));
        client.stop();
        return;
    }
    String method = requestLine.substring(0, firstSpace);
    String path = requestLine.substring(firstSpace + 1, secondSpace);
    if (path.indexOf('?') >= 0) path = path.substring(0, path.indexOf('?'));

    // Header: nur Content-Length wird gebraucht
    long contentLength = 0;
    String line;
    bool complete;
    while ((complete = readLine(client, line, deadline)) && line.length() > 0) {
        if (line.length() > 15 && line.substring(0, 15).equalsIgnoreCase("content-length:")) {
            contentLength = line.substring(15).toInt();
        }
    }
    if (!complete) {
        client.stop();
        return;
    }
    if (contentLength < 0 || contentLength > MAX_BODY_BYTES) {
        sendResponse(client, method, 413, jsonError("Body zu groß"));
        client.stop();
        return;
    }
    String body;
    while ((long)body.length() < contentLength && timeLeft(deadline)) {
        if (client.available()) {
            body += (char)client.read();
        } else if (!client.connected()) {
            break;
        } else {
            delay(1);
        }
    }

    String out;
    int code;
    const char* allow = nullptr;
    if (path == "/config") {
        if (method == "GET") {
            code = 200;
            out = configJson();
        } else if (method == "POST") {
            code = postConfig(body, out);
        } else {
            code = 405;
            out = jsonError("Nur GET und POST erlaubt");
            allow = "GET, POST";
        }
    } else if (path == "/status") {
        if (method == "GET") {
            code = 200;
            out = statusJson();
        } else {
            code = 405;
            out = jsonError("Nur GET erlaubt");
            allow = "GET";
        }
    } else {
        code = 404;
        out = jsonError("Unbekannter Pfad");
    }
    sendResponse(client, method, code, out, allow);
    while (client.available()) client.read();  // Rest verwerfen, sonst sendet der TCP-Stack ein RST
    client.stop();
}

// ---------- Start und Hauptschleife ----------

void setup() {
    Serial.begin(115200);
    delay(500);

    if (!loadSettings()) {
        while (true) {
            Serial.println("settings.toml fehlt: .env ausfuellen und 'pio run -t uploadfs' ausfuehren");
            delay(5000);
        }
    }
    deviceId = setting("device_id");
    telemetryTopic = setting("telemetry_topic");
    humidityTopic = setting("humidity_topic");
    statusTopic = setting("status_topic");
    intervalSeconds = constrain(setting("reading_interval_seconds", "60").toInt(), 2L, (long)MAX_INTERVAL_SECONDS);
    lastWill = "{\"status\":\"offline\",\"sensor_id\":\"" + deviceId + "\"}";

    dht = new DHT(setting("sensor_pin", "4").toInt(), DHT11);
    dht->begin();

    WiFi.begin(setting("wifi_ssid").c_str(), setting("wifi_password").c_str());
    Serial.print("WLAN verbinden mit " + setting("wifi_ssid"));
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nIP: " + WiFi.localIP().toString());

    ntpServer = setting("ntp_server", "pool.ntp.org");
    configTime(0, 0, ntpServer.c_str());  // UTC
    brokerAddress = setting("broker_address");
    mqtt.setServer(brokerAddress.c_str(), setting("broker_port", "1883").toInt());

    httpServer.begin();
    Serial.println("HTTP-API: http://" + WiFi.localIP().toString() + "/status");
}

void loop() {
    keepMqttConnected();
    handleHttp();
    if (lastReading == 0 || millis() - lastReading >= intervalSeconds * 1000) {
        lastReading = millis();
        measureAndPublish();
    }
}
