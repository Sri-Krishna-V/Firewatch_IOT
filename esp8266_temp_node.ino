/*
 * ================================================
 *  FireWatch IoT — ESP8266 Temperature Node
 *  Sensor: DHT11 or DHT22 (GPIO4 / D2)
 *  Topic: fire/temp
 *  Broker: broker.hivemq.com:1883
 * ================================================
 *  Install libraries:
 *    - PubSubClient by Nick O'Leary
 *    - DHT sensor library by Adafruit
 *    - Adafruit Unified Sensor
 * ================================================
 */

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// ── Config ──
const char* WIFI_SSID   = "YOUR_WIFI_SSID";
const char* WIFI_PASS   = "YOUR_WIFI_PASSWORD";
const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const char* MQTT_TOPIC  = "fw2352/temp";       // Unique prefix avoids topic collision on public broker
const char* MQTT_CLIENT = "fw2352_esp8266_temp";

// ── DHT Sensor ──
#define DHTPIN   4        // GPIO4 = D2 on NodeMCU
#define DHTTYPE  DHT11    // DHT11 — max range 0-50°C, thresholds adjusted accordingly
DHT dht(DHTPIN, DHTTYPE);

// ── Pins ──
const int LED_PIN    = LED_BUILTIN;  // D4 / GPIO2 (active LOW on ESP8266)
const int BUZZER_PIN = 5;            // D1 / GPIO5 (optional)

// ── Thresholds ── (calibrated for DHT11 max range 0–50°C)
const float TEMP_ELEVATED = 35.0;   // Warm — worth watching
const float TEMP_WARN     = 42.0;   // Hot — possible fire nearby
const float TEMP_CRITICAL = 48.0;   // Near DHT11 sensor limit — treat as FIRE alert

// ── Timing ──
const unsigned long PUBLISH_INTERVAL = 3000;
unsigned long lastPublish = 0;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

void setup() {
  Serial.begin(115200);
  Serial.println("\n[FireWatch] ESP8266 Temperature Node Starting...");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // Active LOW: HIGH = off

  dht.begin();
  delay(2000);  // DHT sensor warm-up

  connectWiFi();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setKeepAlive(60);

  connectMQTT();
  Serial.println("[FireWatch] Temperature node ready.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  unsigned long now = millis();
  if (now - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = now;
    readAndPublishTemp();
  }
}

void readAndPublishTemp() {
  float temp = dht.readTemperature();     // Celsius
  float humidity = dht.readHumidity();

  // Handle sensor read failure
  if (isnan(temp) || isnan(humidity)) {
    Serial.println("[TEMP] Sensor read failed — skipping publish");
    return;
  }

  Serial.printf("[TEMP] Temperature: %.1f°C | Humidity: %.1f%%\n", temp, humidity);

  // Determine status
  const char* statusStr;
  bool alertFlag = false;

  if (temp >= TEMP_CRITICAL) {
    statusStr = "critical";
    alertFlag = true;
    // Rapid beep + blink
    for (int i = 0; i < 5; i++) {
      digitalWrite(LED_PIN, LOW);   // on
      delay(80);
      digitalWrite(LED_PIN, HIGH);  // off
      delay(80);
    }
  } else if (temp >= TEMP_WARN) {
    statusStr = "high";
    alertFlag = true;
    digitalWrite(LED_PIN, LOW);  // steady on
  } else if (temp >= TEMP_ELEVATED) {
    statusStr = "elevated";
    digitalWrite(LED_PIN, LOW);
  } else {
    statusStr = "normal";
    digitalWrite(LED_PIN, HIGH);  // off
  }

  // Build JSON
  char payload[160];
  snprintf(payload, sizeof(payload),
    "{\"temperature\":%.1f,\"humidity\":%.1f,\"status\":\"%s\",\"alert\":%s}",
    temp, humidity, statusStr, alertFlag ? "true" : "false"
  );

  if (mqttClient.publish(MQTT_TOPIC, payload, false)) {
    Serial.printf("[TEMP] Published → %s : %s\n", MQTT_TOPIC, payload);
  } else {
    Serial.println("[TEMP] Publish FAILED");
  }
}

void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s RSSI: %ddBm\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("\n[WiFi] FAILED — restarting");
    ESP.restart();
  }
}

void connectMQTT() {
  // Set Last Will Testament so broker publishes offline status on unexpected disconnect
  mqttClient.setWill("fw2352/status", "{\"node\":\"esp8266_temp\",\"status\":\"offline\"}", false, 0);

  int attempts = 0;
  while (!mqttClient.connected() && attempts < 5) {
    Serial.printf("[MQTT] Connecting as %s...\n", MQTT_CLIENT);
    if (mqttClient.connect(MQTT_CLIENT)) {
      Serial.println("[MQTT] Connected to HiveMQ!");
      mqttClient.publish("fw2352/status", "{\"node\":\"esp8266_temp\",\"status\":\"online\"}");
    } else {
      Serial.printf("[MQTT] Failed rc=%d — retry in 3s\n", mqttClient.state());
      delay(3000);
      attempts++;
    }
  }

  if (!mqttClient.connected()) {
    Serial.println("[MQTT] All retries exhausted — restarting device...");
    delay(1000);
    ESP.restart();
  }
}
