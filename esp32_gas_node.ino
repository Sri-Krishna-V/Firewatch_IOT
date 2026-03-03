/*
 * ================================================
 *  FireWatch IoT — ESP32 Gas Detection Node
 *  Sensor: MQ-2 or MQ-5 (analog, GPIO34)
 *  Topic: fire/gas
 *  Broker: broker.hivemq.com:1883
 * ================================================
 */

#include <PubSubClient.h>
#include <WiFi.h>

// ── Config ──
const char *WIFI_SSID = "Nothing 2a plus";
const char *WIFI_PASS = "12345678";
const char *MQTT_BROKER = "broker.hivemq.com";
const int MQTT_PORT = 1883;
const char *MQTT_TOPIC =
    "fw2352/gas"; // Unique prefix avoids topic collision on public broker
const char *MQTT_CLIENT = "fw2352_esp32_gas";

// ── Pins ──
const int GAS_SENSOR_PIN = 34; // ADC1_CH6 — analog read
const int LED_BUILTIN_PIN = 2; // Onboard LED
const int BUZZER_PIN = 25;     // Optional buzzer
const int BUZZER_CHANNEL = 0;  // LEDC channel for buzzer (ESP32)

// ── Thresholds ── (auto-calibrated at startup — see calibrateBaseline())
// MQ-2 sensor baseline varies per unit — we measure it during warmup.
int GAS_BASELINE      = 0;   // set by calibrateBaseline()
const int GAS_DELTA_WARN  = 65;  // ADC rise above baseline = warning
const int GAS_DELTA_ALERT = 105; // ADC rise above baseline = gas leak
const int GAS_SCALE_RANGE = 200; // delta range mapped to 0–1023 for dashboard

// ── Timing ──
const unsigned long PUBLISH_INTERVAL = 2000; // ms
unsigned long lastPublish = 0;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

void setup() {
  Serial.begin(115200);
  Serial.println("\n[FireWatch] ESP32 Gas Detection Node Starting...");

  pinMode(LED_BUILTIN_PIN, OUTPUT);
  // Set up LEDC for buzzer (ESP32 PWM — replaces tone())
  ledcAttach(BUZZER_PIN, 2000,
             8); // ESP32 Core v3.x API: attach pin, freq 2kHz, 8-bit resolution
  ledcWriteTone(BUZZER_PIN, 0); // silent (v3.x uses pin, not channel)

  connectWiFi();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(10);

  // Calibrate baseline BEFORE connecting MQTT so first publish is accurate
  calibrateBaseline();

  connectMQTT();
  Serial.println("[FireWatch] Gas node ready.");
}

// ── Baseline auto-calibration ─────────────────────────────────────
void calibrateBaseline() {
  Serial.println("[GAS] Calibrating baseline — sensor warming up (30s). Keep area clear of gas/smoke!");
  // Blink slowly during calibration
  long sum = 0;
  const int SAMPLES = 60;  // one sample every 500ms = 30 seconds
  for (int i = 0; i < SAMPLES; i++) {
    sum += analogRead(GAS_SENSOR_PIN);
    digitalWrite(LED_BUILTIN_PIN, i % 2);
    Serial.printf("[GAS] Calibration %d/%d  raw=%d\n", i + 1, SAMPLES, (int)analogRead(GAS_SENSOR_PIN));
    delay(500);
  }
  GAS_BASELINE = sum / SAMPLES;
  // Add a small safety margin so slight sensor drift doesn’t cause false alerts
  GAS_BASELINE += 10;
  Serial.printf("[GAS] Baseline set to %d  warn>%d  alert>%d\n",
                GAS_BASELINE,
                GAS_BASELINE + GAS_DELTA_WARN,
                GAS_BASELINE + GAS_DELTA_ALERT);
  digitalWrite(LED_BUILTIN_PIN, LOW);
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

  // Normalize 12-bit ADC to 0-1023 dashboard scale.
  // Subtract clean-air baseline so safe air = ~0, gas leak = 300-1023.
  int delta = constrain(adcValue - GAS_BASELINE, 0, GAS_SCALE_RANGE);
  int normalizedValue = map(delta, 0, GAS_SCALE_RANGE, 0, 1023);

  Serial.printf("[GAS] raw=%d  baseline=%d  delta=%d  normalized=%d\n",
                adcValue, GAS_BASELINE, delta, normalizedValue);

  // Build JSON payload
  char payload[128];
  const char *statusStr;
  bool isAlert = false;

  if (adcValue > GAS_BASELINE + GAS_DELTA_ALERT) {
    statusStr = "leak";
    isAlert = true;
    // Alert: blink fast + buzzer
    for (int i = 0; i < 3; i++) {
      digitalWrite(LED_BUILTIN_PIN, HIGH);
      ledcWriteTone(BUZZER_PIN, 2000); // 2kHz beep
      delay(100);
      digitalWrite(LED_BUILTIN_PIN, LOW);
      ledcWriteTone(BUZZER_PIN, 0); // silent
      delay(100);
    }
  } else if (adcValue > GAS_BASELINE + GAS_DELTA_WARN) {
    statusStr = "warning";
    digitalWrite(LED_BUILTIN_PIN, HIGH);
  } else {
    statusStr = "safe";
    digitalWrite(LED_BUILTIN_PIN, LOW);
    ledcWriteTone(BUZZER_PIN, 0); // silence buzzer
  }

  snprintf(payload, sizeof(payload),
           "{\"value\":%d,\"raw\":%d,\"status\":\"%s\",\"alert\":%s}",
           normalizedValue, adcValue, statusStr,
           isAlert ? "true" : "false");

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
    Serial.printf("\n[WiFi] Connected! IP: %s\n",
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Connection FAILED — restarting...");
    ESP.restart();
  }
}

void connectMQTT() {
  // LWT: broker auto-publishes this if ESP32 disconnects unexpectedly
  // setWill() does NOT exist in PubSubClient — pass it into connect() instead
  const char *willTopic = "fw2352/status";
  const char *willMsg = "{\"node\":\"esp32_gas\",\"status\":\"offline\"}";

  int attempts = 0;
  while (!mqttClient.connected() && attempts < 5) {
    Serial.printf("[MQTT] Connecting as %s...\n", MQTT_CLIENT);
    // connect(id, user, pass, willTopic, willQos, willRetain, willMsg)
    if (mqttClient.connect(MQTT_CLIENT, nullptr, nullptr, willTopic, 0, false,
                           willMsg)) {
      Serial.println("[MQTT] Connected to HiveMQ!");
      mqttClient.publish("fw2352/status",
                         "{\"node\":\"esp32_gas\",\"status\":\"online\"}");
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
