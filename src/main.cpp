
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include "secrets.h"

// Hardware
#define DHT_PIN 15
#define DHT_TYPE DHT22
#define LED_PIN 2

// Wi-Fi
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// Azure API
const char* API_URL =
    "http://iot-skyethomas.mexicocentral.cloudapp.azure.com/readings";

// MQTT broker
const char* MQTT_SERVER = "135.232.203.144";
const int MQTT_PORT = 1883;

// IMPORTANT: Match MQTT_NAME in your Azure .env
const char* MQTT_NAME = "skyethomas";

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

String mac;
String baseTopic;

float alarmLimit = 30.0;
float latestTemperature = NAN;
bool alarmActive = false;
bool alarmPublished = false;

unsigned long lastReading = 0;
unsigned long lastMqttAttempt = 0;

const unsigned long READING_INTERVAL = 10000;
const unsigned long MQTT_RETRY_INTERVAL = 5000;

// Update LED and publish alarm state when needed
void updateAlarm() {
    if (!isfinite(latestTemperature)) {
        return;
    }

    bool newState = latestTemperature > alarmLimit;

    digitalWrite(LED_PIN, newState ? HIGH : LOW);

    if (newState != alarmActive || !alarmPublished) {
        alarmActive = newState;

        if (mqtt.connected()) {
            String topic = baseTopic + "/alarm/state";

            bool published = mqtt.publish(
                topic.c_str(),
                alarmActive ? "ON" : "OFF",
                true
            );

            if (published) {
                alarmPublished = true;
            }
        }

        Serial.println(
            alarmActive ? "ALARM ON" : "ALARM OFF"
        );
    }
}

// Receive a new alarm limit from MQTT
void mqttCallback(
    char* topic,
    byte* payload,
    unsigned int length
) {
    String message;

    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }

    message.trim();

    if (message.length() == 0) {
        Serial.println("Invalid alarm limit");
        return;
    }

    char* endPointer;
    float newLimit = strtof(
        message.c_str(),
        &endPointer
    );

    while (*endPointer &&
           isspace((unsigned char)*endPointer)) {
        endPointer++;
    }

    if (endPointer == message.c_str() ||
        *endPointer != '\0' ||
        !isfinite(newLimit)) {
        Serial.println("Invalid alarm limit");
        return;
    }

    alarmLimit = newLimit;

    Serial.printf(
        "New alarm limit: %.2f\n",
        alarmLimit
    );

    // Immediately update LED using latest reading
    updateAlarm();
}

// Connect to MQTT
void connectMQTT() {
    if (mqtt.connected()) {
        return;
    }

    if (millis() - lastMqttAttempt <
        MQTT_RETRY_INTERVAL) {
        return;
    }

    lastMqttAttempt = millis();

    String clientId = "esp32-" + mac;
    String statusTopic = baseTopic + "/status";
    String limitTopic = baseTopic + "/alarm/max";

    bool connected = mqtt.connect(
        clientId.c_str(),
        statusTopic.c_str(),
        0,
        true,
        "offline"
    );

    if (connected) {
        Serial.println("MQTT connected");

        // Publish retained online status
        mqtt.publish(
            statusTopic.c_str(),
            "online",
            true
        );

        // Subscribe to alarm limit
        mqtt.subscribe(limitTopic.c_str());

        // Republish alarm state after reconnect
        alarmPublished = false;
        updateAlarm();
    } else {
        Serial.printf(
            "MQTT connection failed: %d\n",
            mqtt.state()
        );
    }
}

// Send temperature to Azure
void sendReading(float temperature) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi disconnected");
        return;
    }

    HTTPClient http;

    http.begin(API_URL);

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    http.addHeader(
        "X-API-Key",
        DEVICE_KEY
    );

    // CORRECT FIELD NAME: "temp"
    String json = "{\"mac\":\"" + mac +
                  "\",\"temp\":" +
                  String(temperature, 2) + "}";

    int response = http.POST(json);

    Serial.println("Sent: " + json);
    Serial.println(
        "API response: " + http.getString()
    );

    Serial.printf(
        "Temperature: %.2f, HTTP: %d\n",
        temperature,
        response
    );

    http.end();
}

void setup() {
    Serial.begin(115200);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    dht.begin();

    // Connect Wi-Fi
    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    Serial.print("Connecting to Wi-Fi");

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("\nWi-Fi connected");

    mac = WiFi.macAddress();

    Serial.println("MAC: " + mac);

    baseTopic = String("ecse3038/") + MQTT_NAME;

    mqtt.setServer(
        MQTT_SERVER,
        MQTT_PORT
    );

    mqtt.setCallback(mqttCallback);

    // Allow retained alarm messages
    mqtt.setBufferSize(256);

    // First MQTT connection attempt
    lastMqttAttempt =
        millis() - MQTT_RETRY_INTERVAL;

    connectMQTT();

    // Allow first temperature reading immediately
    lastReading =
        millis() - READING_INTERVAL;
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
        delay(500);
        return;
    }

    if (!mqtt.connected()) {
        connectMQTT();
    }

    mqtt.loop();

    // Read every 10 seconds
    if (millis() - lastReading >=
        READING_INTERVAL) {

        lastReading = millis();

        float temperature =
            dht.readTemperature();

        if (isnan(temperature)) {
            Serial.println("DHT22 read failed");
            return;
        }

        latestTemperature = temperature;

        // Update alarm LED
        updateAlarm();

        // Send to Azure
        sendReading(temperature);
    }
}
