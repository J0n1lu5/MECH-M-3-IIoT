// Minimalversion: DHT11 auslesen, Werte per MQTT publishen und per REST (GET /data) bereitstellen.
// WIFI_SSID, WIFI_PASSWORD, MQTT_BROKER_HOST, MQTT_USER, MQTT_PASSWORD kommen aus .env (load_env.py).
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

const int MQTT_PORT = 1883;
const unsigned long SEND_INTERVAL = 5000; // ms

DHT dht(4, DHT11);
WiFiClient espClient;
PubSubClient mqtt(espClient);
WebServer server(80);

String sensorId;
String topic;
float temperature = NAN;
float humidity = NAN;
unsigned long lastSend = 0;

String buildJson() {
    JsonDocument doc;
    doc["sensorId"] = sensorId;
    doc["timestamp"] = millis();
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    String out;
    serializeJson(doc, out);
    return out;
}

void reconnectMqtt() {
    while (!mqtt.connected()) {
        Serial.print("MQTT connect...");
        if (mqtt.connect(sensorId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
            Serial.println("ok");
        } else {
            Serial.printf("failed, rc=%d, retry in 5s\n", mqtt.state());
            delay(5000);
        }
    }
}

void setup() {
    Serial.begin(115200);
    dht.begin();

    char chipId[13];
    snprintf(chipId, sizeof(chipId), "%012llX", ESP.getEfuseMac());
    sensorId = String("ESP32_Sensor_") + chipId;
    topic = "iiot/sensors/" + sensorId + "/data";

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("WiFi connect");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nIP: " + WiFi.localIP().toString());

    mqtt.setServer(MQTT_BROKER_HOST, MQTT_PORT);

    // REST: GET /data -> letzter Messwert als JSON
    server.on("/data", HTTP_GET, []() { server.send(200, "application/json", buildJson()); });
    server.begin();
    Serial.println("REST: http://" + WiFi.localIP().toString() + "/data");
    Serial.println("MQTT topic: " + topic);
}

void loop() {
    if (!mqtt.connected()) reconnectMqtt();
    mqtt.loop();
    server.handleClient();

    if (millis() - lastSend >= SEND_INTERVAL) {
        lastSend = millis();
        temperature = dht.readTemperature();
        humidity = dht.readHumidity();
        if (isnan(temperature)) {
            Serial.println("DHT read failed");
            return;
        }
        String payload = buildJson();
        bool ok = mqtt.publish(topic.c_str(), payload.c_str());
        Serial.println((ok ? "Published: " : "Publish failed: ") + payload);
    }
}
