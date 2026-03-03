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

#include <DHT.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// ── Config ──
const char *WIFI_SSID = "Nothing 2a plus";
const char *WIFI_PASS = "12345678";
const char *MQTT_BROKER = "broker.hivemq.com";
const int MQTT_PORT = 1883;
const char *MQTT_TOPIC =
    "fw2352/temp"; // Unique prefix avoids topic collision on public broker
const char *MQTT_CLIENT = "fw2352_esp8266_temp";

// ── DHT Sensor ──
#define DHTPIN 14 // GPIO14 = D5 on NodeMCU (changed from D2 — more reliable)
#define DHTTYPE DHT11 // DHT11: 0–50°C range
DHT dht(DHTPIN, DHTTYPE);

// ── Pins ──
const int LED_PIN = LED_BUILTIN; // D4 / GPIO2 (active LOW on ESP8266)
const int BUZZER_PIN = 5;        // D1 / GPIO5 (optional)

// ── Thresholds ── (calibrated for DHT11 max 0–50°C)
const float TEMP_ELEVATED = 30.0; // Slightly warm
const float TEMP_WARN = 38.0;     // Hot — possible fire nearby
const float TEMP_CRITICAL = 45.0; // Near DHT11 limit — treat as FIRE alert

// ── Timing ──
const unsigned long PUBLISH_INTERVAL = 3000;
unsigned long lastPublish = 0;

// ── Edge Computation: Exponential Moving Average (EMA) ────────────────────
// α=0.3 gives ~3-cycle settling — appropriate for DHT11 slow sample rate
const float EMA_ALPHA = 0.3f;
float emaTemp     = NAN;   // NAN signals "not seeded yet"
float emaHumidity = NAN;

// ── Edge Computation: Rate-of-Change ─────────────────────────────────────
float prevEmaTemp = NAN;
const float ROC_RISING_THRESHOLD = 1.5f; // °C per 3 s cycle ⇒ rising fast

// ── Edge Computation: Delta-based Conditional Publish ────────────────────
float        lastPubTemp     = NAN;
float        lastPubHumidity = NAN;
const char  *lastPubStatus   = "";
unsigned long lastForcedPub  = 0;
const unsigned long FORCED_PUB_INTERVAL = 30000; // 30 s heartbeat
const float TEMP_DELTA_THR = 0.5f;   // °C
const float HUM_DELTA_THR  = 2.0f;   // %

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ── Heat Index (Rothfusz regression, NOAA) ───────────────────────────────
// Returns apparent temperature (°C). Valid for T≥27°C and RH≥40%;
// returns raw temperature below that range (sensor accurate enough there).
float computeHeatIndex(float T, float RH) {
  if (T < 27.0f || RH < 40.0f) return T;
  return -8.78469475556f
         + 1.61139411f   * T
         + 2.33854883889f * RH
         - 0.14611605f   * T  * RH
         - 0.01230809050f * T  * T
         - 0.01642482777f * RH * RH
         + 0.00221732f   * T  * T  * RH
         + 0.00072546f   * T  * RH * RH
         - 0.00000358528f * T  * T  * RH * RH;
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n[FireWatch] ESP8266 Temperature Node Starting...");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Active LOW: HIGH = off

  // Enable internal pull-up on DHT data pin (use if no external 10kΩ resistor)
  pinMode(DHTPIN, INPUT_PULLUP);

  dht.begin();
  delay(3000); // DHT11 needs at least 2-3s to stabilize after power-on

  connectWiFi();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setKeepAlive(60);

  connectMQTT();
  Serial.println("[FireWatch] Temperature node ready.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED)
    connectWiFi();
  if (!mqttClient.connected())
    connectMQTT();
  mqttClient.loop();

  unsigned long now = millis();
  if (now - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = now;
    readAndPublishTemp();
  }
}

void readAndPublishTemp() {
  float rawTemp     = dht.readTemperature();
  float rawHumidity = dht.readHumidity();

  if (isnan(rawTemp) || isnan(rawHumidity)) {
    Serial.println("[TEMP] Sensor read failed — skipping publish");
    return;
  }

  // ── Step 1: EMA smoothing ─────────────────────────────────────────────
  if (isnan(emaTemp)) {
    emaTemp     = rawTemp;       // seed on first valid read
    emaHumidity = rawHumidity;
  } else {
    emaTemp     = EMA_ALPHA * rawTemp     + (1.0f - EMA_ALPHA) * emaTemp;
    emaHumidity = EMA_ALPHA * rawHumidity + (1.0f - EMA_ALPHA) * emaHumidity;
  }

  // ── Step 2: Heat Index ────────────────────────────────────────────────
  float heatIndex = computeHeatIndex(emaTemp, emaHumidity);

  // ── Step 3: Rate-of-change ─────────────────────────────────────────
  bool rising = false;
  if (!isnan(prevEmaTemp)) {
    rising = (emaTemp - prevEmaTemp) > ROC_RISING_THRESHOLD;
  }
  prevEmaTemp = emaTemp;

  // ── Step 4: Status — use heat index + early-warn if rising ────────────
  // When rising fast, bump the effective temperature up by 5°C so we
  // escalate one severity tier earlier.
  float effectiveTemp = rising ? (emaTemp + 5.0f) : emaTemp;
  const char *statusStr;
  bool alertFlag = false;

  if (heatIndex >= TEMP_CRITICAL || effectiveTemp >= TEMP_CRITICAL) {
    statusStr = "critical";
    alertFlag = true;
    for (int i = 0; i < 5; i++) {
      digitalWrite(LED_PIN, LOW);
      delay(80);
      digitalWrite(LED_PIN, HIGH);
      delay(80);
    }
  } else if (heatIndex >= TEMP_WARN || effectiveTemp >= TEMP_WARN) {
    statusStr = "high";
    alertFlag = true;
    digitalWrite(LED_PIN, LOW);
  } else if (emaTemp >= TEMP_ELEVATED) {
    statusStr = "elevated";
    digitalWrite(LED_PIN, LOW);
  } else {
    statusStr = "normal";
    digitalWrite(LED_PIN, HIGH);
  }

  // ── Step 5: Risk score 0–100 based on heat index ─────────────────────
  // 20°C = 0 risk, 80°C+ = 100 risk
  int riskScore = (int)constrain((heatIndex - 20.0f) * 100.0f / 60.0f, 0.0f, 100.0f);

  // ── Step 6: Delta-based conditional publish ───────────────────────────
  unsigned long now = millis();
  bool statusChanged = (strcmp(statusStr, lastPubStatus) != 0);
  bool tempDrifted   = isnan(lastPubTemp)     || (fabs(emaTemp     - lastPubTemp)     > TEMP_DELTA_THR);
  bool humDrifted    = isnan(lastPubHumidity) || (fabs(emaHumidity - lastPubHumidity) > HUM_DELTA_THR);
  bool heartbeatDue  = (now - lastForcedPub >= FORCED_PUB_INTERVAL);

  Serial.printf("[TEMP] raw=%.1f°C ema=%.1f°C hi=%.1f°C hum=%.1f%% %s status=%s risk=%d\n",
                rawTemp, emaTemp, heatIndex, emaHumidity,
                rising ? "RISING" : "", statusStr, riskScore);

  if (!statusChanged && !tempDrifted && !humDrifted && !heartbeatDue) {
    Serial.println("[TEMP] Suppressed — no significant change");
    return;
  }

  lastPubTemp     = emaTemp;
  lastPubHumidity = emaHumidity;
  lastPubStatus   = statusStr;
  lastForcedPub   = now;

  // ── Step 7: Build + publish JSON ─────────────────────────────────────
  char payload[200];
  snprintf(payload, sizeof(payload),
           "{\"temperature\":%.1f,\"humidity\":%.1f,\"heat_index\":%.1f,"
           "\"status\":\"%s\",\"trend\":\"%s\",\"alert\":%s,\"risk\":%d}",
           emaTemp, emaHumidity, heatIndex,
           statusStr, rising ? "rising" : "stable",
           alertFlag ? "true" : "false", riskScore);

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
  // LWT: broker auto-publishes this if ESP8266 disconnects unexpectedly
  // setWill() does NOT exist in PubSubClient — pass it into connect() instead
  const char *willTopic = "fw2352/status";
  const char *willMsg = "{\"node\":\"esp8266_temp\",\"status\":\"offline\"}";

  int attempts = 0;
  while (!mqttClient.connected() && attempts < 5) {
    Serial.printf("[MQTT] Connecting as %s...\n", MQTT_CLIENT);
    // connect(id, user, pass, willTopic, willQos, willRetain, willMsg)
    if (mqttClient.connect(MQTT_CLIENT, nullptr, nullptr, willTopic, 0, false,
                           willMsg)) {
      Serial.println("[MQTT] Connected to HiveMQ!");
      mqttClient.publish("fw2352/status",
                         "{\"node\":\"esp8266_temp\",\"status\":\"online\"}");
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
