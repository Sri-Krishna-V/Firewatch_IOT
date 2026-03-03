/*
 * ================================================
 *  FireWatch IoT — ESP32 Gas Detection Node
 *  Sensor: MQ-2 or MQ-5 (analog, GPIO34)
 *  Topic: fire/gas
 *  Broker: broker.hivemq.com:1883
 * ================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>

// ── Config ──
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASS     = "YOUR_WIFI_PASSWORD";
const char* MQTT_BROKER   = "broker.hivemq.com";
const int   MQTT_PORT     = 1883;
const char* MQTT_TOPIC    = "fire/gas";
const char* MQTT_CLIENT   = "esp32_gas_node_001";

// ── Pins ──
const int GAS_SENSOR_PIN  = 34;   // ADC1_CH6 — analog read
const int LED_BUILTIN_PIN = 2;    // Onboard LED
const int BUZZER_PIN      = 25;   // Optional buzzer

// ── Thresholds ──
const int GAS_WARN_THRESHOLD  = 200;
const int GAS_ALERT_THRESHOLD = 400;

// ── Timing ──
const unsigned long PUBLISH_INTERVAL = 2000;  // ms
unsigned long lastPublish = 0;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

void setup() {
  Serial.begin(115200);
  Serial.println("\n[FireWatch] ESP32 Gas Detection Node Starting...");

  pinMode(LED_BUILTIN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_BUILTIN_PIN, LOW);
  noTone(BUZZER_PIN);

  connectWiFi();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(10);

  connectMQTT();
  Serial.println("[FireWatch] Gas node ready.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected — reconnecting...");
    connectWiFi();
  }
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  unsigned long now = millis();
  if (now - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = now;
    readAndPublishGas();
  }
}

void readAndPublishGas() {
  // Average 5 readings for stability
  long sum = 0;
  for (int i = 0; i < 5; i++) {
    sum += analogRead(GAS_SENSOR_PIN);
    delay(10);
  }
  int adcValue = sum / 5;

  Serial.printf("[GAS] ADC Value: %d\n", adcValue);

  // Build JSON payload
  char payload[128];
  const char* statusStr;
  bool isAlert = false;

  if (adcValue > GAS_ALERT_THRESHOLD) {
    statusStr = "leak";
    isAlert = true;
    // Alert: blink fast + buzzer
    for (int i = 0; i < 3; i++) {
      digitalWrite(LED_BUILTIN_PIN, HIGH);
      tone(BUZZER_PIN, 2000, 100);
      delay(100);
      digitalWrite(LED_BUILTIN_PIN, LOW);
      delay(100);
    }
  } else if (adcValue > GAS_WARN_THRESHOLD) {
    statusStr = "warning";
    digitalWrite(LED_BUILTIN_PIN, HIGH);
  } else {
    statusStr = "safe";
    digitalWrite(LED_BUILTIN_PIN, LOW);
    noTone(BUZZER_PIN);
  }

  snprintf(payload, sizeof(payload),
    "{\"value\":%d,\"status\":\"%s\",\"alert\":%s}",
    adcValue, statusStr, isAlert ? "true" : "false"
  );

  if (mqttClient.publish(MQTT_TOPIC, payload, false)) {
    Serial.printf("[GAS] Published → %s : %s\n", MQTT_TOPIC, payload);
  } else {
    Serial.println("[GAS] Publish FAILED");
  }
}

void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Connection FAILED — restarting...");
    ESP.restart();
  }
}

void connectMQTT() {
  // Set Last Will Testament so broker publishes offline status on unexpected disconnect
  mqttClient.setWill("fire/status", "{\"node\":\"esp32_gas\",\"status\":\"offline\"}", false, 0);

  int attempts = 0;
  while (!mqttClient.connected() && attempts < 5) {
    Serial.printf("[MQTT] Connecting as %s...\n", MQTT_CLIENT);
    if (mqttClient.connect(MQTT_CLIENT)) {
      Serial.println("[MQTT] Connected to HiveMQ!");
      // Publish online status
      mqttClient.publish("fire/status", "{\"node\":\"esp32_gas\",\"status\":\"online\"}");
    } else {
      Serial.printf("[MQTT] Failed, rc=%d — retry in 3s\n", mqttClient.state());
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
