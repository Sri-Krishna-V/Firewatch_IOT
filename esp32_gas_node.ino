/*
 * ════════════════════════════════════════════════
 *  FireWatch IoT — ESP32 Gas Detection Node
 *  Sensor : MQ-6 (analog, GPIO34)
 *  Topic  : <prefix>/gas   (prefix set via config portal)
 *  Broker : configurable — captive portal on first boot
 * ════════════════════════════════════════════════
 *  Libraries (Arduino Library Manager):
 *    - PubSubClient  by Nick O'Leary
 *    - WiFiManager   by tzapu/tablatronix
 * ════════════════════════════════════════════════
 *  First-boot setup:
 *    1. Power on → AP "FireWatch-Gas" appears
 *    2. Connect phone/laptop to that AP
 *    3. Browser opens portal at 192.168.4.1
 *    4. Enter WiFi SSID/password, MQTT broker host,
 *       port, and topic prefix → click Save
 *    5. Device reboots and connects automatically
 *    6. Config stored in NVS (survives power cycles)
 *    7. To reconfigure: hold BOOT button 3 s
 * ════════════════════════════════════════════════
 */

#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiManager.h>

// ── Runtime config (loaded from NVS via Preferences) ─────────────────────
struct FWConfig {
  char mqtt_broker[64];
  int  mqtt_port;
  char topic_prefix[32];
};
FWConfig cfg;

// Derived MQTT topics — rebuilt after loadConfig()
char MQTT_TOPIC[48];
char MQTT_STATUS_TOPIC[48];
const char *MQTT_CLIENT = "fw2352_esp32_gas";

Preferences prefs;

void loadConfig() {
  prefs.begin("fw_gas", true);   // read-only namespace
  String broker = prefs.getString("broker", "broker.hivemq.com");
  cfg.mqtt_port  = prefs.getInt   ("port",    1883);
  String prefix  = prefs.getString("prefix",  "fw2352");
  prefs.end();
  broker.toCharArray(cfg.mqtt_broker,  sizeof(cfg.mqtt_broker));
  prefix.toCharArray(cfg.topic_prefix, sizeof(cfg.topic_prefix));
  snprintf(MQTT_TOPIC,        sizeof(MQTT_TOPIC),        "%s/gas",    cfg.topic_prefix);
  snprintf(MQTT_STATUS_TOPIC, sizeof(MQTT_STATUS_TOPIC), "%s/status", cfg.topic_prefix);
  Serial.printf("[CFG] broker=%s  port=%d  prefix=%s\n",
                cfg.mqtt_broker, cfg.mqtt_port, cfg.topic_prefix);
}

void saveConfig() {
  prefs.begin("fw_gas", false);  // read-write
  prefs.putString("broker", cfg.mqtt_broker);
  prefs.putInt   ("port",   cfg.mqtt_port);
  prefs.putString("prefix", cfg.topic_prefix);
  prefs.end();
  Serial.println("[CFG] Config saved to NVS.");
}

// ── Pins ──
const int GAS_SENSOR_PIN = 34; // ADC1_CH6 — analog read
const int LED_BUILTIN_PIN = 2; // Onboard LED
const int BUZZER_PIN = 25;     // Optional buzzer
const int BUZZER_CHANNEL = 0;  // LEDC channel for buzzer (ESP32)

// ── Thresholds ── (auto-calibrated at startup — see calibrateBaseline())
// MQ-6 sensor baseline varies per unit — we measure it during warmup.
int GAS_BASELINE      = 0;   // set by calibrateBaseline()
const int GAS_DELTA_WARN  = 65;  // ADC rise above baseline = warning
const int GAS_DELTA_ALERT = 105; // ADC rise above baseline = gas leak
const int GAS_SCALE_RANGE = 200; // delta range mapped to 0–1023 for dashboard

// ── Timing ──
const unsigned long PUBLISH_INTERVAL = 2000; // ms
unsigned long lastPublish = 0;

// ── Edge Computation: Ring-buffer Moving Average ─────────────────────────
const int MA_SIZE = 10;                 // slots — 10 × 2 s = 20 s window
uint16_t gasBuf[MA_SIZE] = {0};
int gaBufIdx  = 0;
bool gaBufFull = false;

// ── Edge Computation: Rate-of-Change ─────────────────────────────────────
int  gaPrevSmoothed    = -1;           // -1 = not seeded yet (skip ROC on first cycle)
const int ROC_RISING_THRESHOLD = 20;   // ADC units per cycle ⇒ considered rising

// ── Edge Computation: Delta-based Conditional Publish ────────────────────
int          gaLastPublishedValue  = -999;
const char  *gaLastPublishedStatus = "";
unsigned long gaLastForcedPublish  = 0;
const unsigned long FORCED_PUB_INTERVAL = 30000; // 30 s heartbeat
const int    DELTA_THRESHOLD = 5;      // suppress if norm value drifts ≤ this

// ── Offline queue: buffer up to 10 payloads when broker is unreachable ─────
#define OFFLINE_QUEUE_SIZE 10
String offlineQueue[OFFLINE_QUEUE_SIZE];
int    oqHead  = 0;   // index of oldest entry
int    oqTail  = 0;   // index where next entry is written
int    oqCount = 0;   // number of buffered entries

void queuePayload(const char *payload) {
  offlineQueue[oqTail] = payload;
  oqTail = (oqTail + 1) % OFFLINE_QUEUE_SIZE;
  if (oqCount < OFFLINE_QUEUE_SIZE) {
    oqCount++;
  } else {
    oqHead = (oqHead + 1) % OFFLINE_QUEUE_SIZE; // overwrite oldest
    Serial.println("[Q] Buffer full — oldest entry overwritten");
  }
  Serial.printf("[Q] Queued (#%d): %s\n", oqCount, payload);
}

void flushQueue();

WiFiClient espClient;
PubSubClient mqttClient(espClient);

void setup() {
  Serial.begin(115200);
  Serial.println("\n[FireWatch] ESP32 Gas Detection Node Starting...");

  pinMode(LED_BUILTIN_PIN, OUTPUT);
  // Set up LEDC for buzzer (ESP32 PWM — replaces tone())
  ledcAttach(BUZZER_PIN, 2000,
             8); // ESP32 Core v3.x API: attach pin, freq 2kHz, 8-bit resolution
  ledcWriteTone(BUZZER_PIN, 0); // silent

  // Load saved broker/prefix config from NVS (or defaults on first boot)
  loadConfig();
  // connectWiFi() launches WiFiManager portal if no credentials are stored
  connectWiFi();

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
    // In-loop reconnect: use WiFi.reconnect() — NOT the full portal
    Serial.println("[WiFi] Disconnected — reconnecting...");
    WiFi.reconnect();
    unsigned long _t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - _t < 10000) delay(200);
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
  // ── Step 1: Read ADC + ring-buffer moving average ─────────────────────
  long sum = 0;
  for (int i = 0; i < 5; i++) {
    sum += analogRead(GAS_SENSOR_PIN);
    delay(10);
  }
  int adcValue = sum / 5;

  // Fill ring buffer one slot per publish cycle
  gasBuf[gaBufIdx] = (uint16_t)adcValue;
  gaBufIdx = (gaBufIdx + 1) % MA_SIZE;
  if (gaBufIdx == 0) gaBufFull = true;

  int filledSlots = gaBufFull ? MA_SIZE : gaBufIdx;
  long bufSum = 0;
  for (int i = 0; i < filledSlots; i++) bufSum += gasBuf[i];
  int smoothed = (int)(bufSum / filledSlots);

  // ── Step 2: Normalize + risk score (0–100) ────────────────────────────
  int delta          = constrain(smoothed - GAS_BASELINE, 0, GAS_SCALE_RANGE);
  int normalizedValue = map(delta, 0, GAS_SCALE_RANGE, 0, 1023);
  int riskScore      = (int)map(delta, 0, GAS_SCALE_RANGE, 0, 100);

  // ── Step 3: Rate-of-change (per publish cycle ~2 s) ──────────────────
  int  rateOfChange = 0;
  bool rising       = false;
  if (gaPrevSmoothed >= 0) {
    // Skip ROC on very first cycle — gaPrevSmoothed=0 would give a false RISING spike
    rateOfChange = smoothed - gaPrevSmoothed;
    rising       = (rateOfChange > ROC_RISING_THRESHOLD);
  }
  gaPrevSmoothed = smoothed;

  // ── Step 4: Status classification with early-warn on rising trend ─────
  const char *statusStr;
  bool isAlert = false;

  if (smoothed > GAS_BASELINE + GAS_DELTA_ALERT) {
    statusStr = "leak";
    isAlert   = true;
    for (int i = 0; i < 3; i++) {
      digitalWrite(LED_BUILTIN_PIN, HIGH);
      ledcWriteTone(BUZZER_PIN, 2000);
      delay(100);
      digitalWrite(LED_BUILTIN_PIN, LOW);
      ledcWriteTone(BUZZER_PIN, 0);
      delay(100);
    }
  } else if (smoothed > GAS_BASELINE + GAS_DELTA_WARN || rising) {
    // Rising fast below warn threshold → early-warn
    statusStr = "warning";
    digitalWrite(LED_BUILTIN_PIN, HIGH);
  } else {
    statusStr = "safe";
    digitalWrite(LED_BUILTIN_PIN, LOW);
    ledcWriteTone(BUZZER_PIN, 0);
  }

  // ── Step 5: Delta-based conditional publish ───────────────────────────
  unsigned long now       = millis();
  bool statusChanged      = (strcmp(statusStr, gaLastPublishedStatus) != 0);
  bool valueDrifted       = (abs(normalizedValue - gaLastPublishedValue) > DELTA_THRESHOLD);
  bool heartbeatDue       = (now - gaLastForcedPublish >= FORCED_PUB_INTERVAL);

  Serial.printf("[GAS] raw=%d smooth=%d base=%d delta=%d norm=%d risk=%d roc=%+d %s\n",
                adcValue, smoothed, GAS_BASELINE, delta, normalizedValue,
                riskScore, rateOfChange, rising ? "RISING" : "");

  if (!statusChanged && !valueDrifted && !heartbeatDue) {
    Serial.println("[GAS] Suppressed — no significant change");
    return;
  }

  gaLastPublishedValue  = normalizedValue;
  gaLastPublishedStatus = statusStr;
  gaLastForcedPublish   = now;

  // ── Step 6: Build + publish JSON ─────────────────────────────────────
  char payload[192];
  snprintf(payload, sizeof(payload),
           "{\"value\":%d,\"raw\":%d,\"status\":\"%s\","
           "\"trend\":\"%s\",\"alert\":%s,\"risk\":%d}",
           normalizedValue, adcValue, statusStr,
           rising ? "rising" : "stable",
           isAlert ? "true" : "false",
           riskScore);

  if (mqttClient.connected()) {
    if (mqttClient.publish(MQTT_TOPIC, payload, false)) {
      Serial.printf("[GAS] Published → %s : %s\n", MQTT_TOPIC, payload);
    } else {
      Serial.println("[GAS] Publish FAILED — queuing");
      queuePayload(payload);
    }
  } else {
    queuePayload(payload);
  }
}

// ── Offline queue flush ────────────────────────────────────────────────────
void flushQueue() {
  if (oqCount == 0) return;
  Serial.printf("[Q] Flushing %d buffered payload(s)...\n", oqCount);
  int flushed = 0;
  while (oqCount > 0 && mqttClient.connected()) {
    if (mqttClient.publish(MQTT_TOPIC, offlineQueue[oqHead].c_str(), false)) {
      flushed++;
    } else {
      Serial.println("[Q] Flush publish failed — aborting");
      break;
    }
    oqHead = (oqHead + 1) % OFFLINE_QUEUE_SIZE;
    oqCount--;
  }
  Serial.printf("[Q] Flushed %d payload(s).\n", flushed);
}

void connectWiFi() {
  // ── WiFiManager captive-portal provisioning ────────────────────────────
  // First boot: broadcasts AP "FireWatch-Gas". Connect → 192.168.4.1 →
  // fill in SSID, password, broker host, port, topic prefix → Save.
  // Subsequent boots: reconnects silently using stored credentials.
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);  // close portal after 3 min if unused

  // Custom parameters bound to runtime config
  WiFiManagerParameter p_broker("broker", "MQTT Broker Host",
                                 cfg.mqtt_broker,  63);
  char portBuf[6]; snprintf(portBuf, sizeof(portBuf), "%d", cfg.mqtt_port);
  WiFiManagerParameter p_port  ("port",   "MQTT Port (TCP)",
                                 portBuf,           5);
  WiFiManagerParameter p_prefix("prefix", "Topic Prefix",
                                 cfg.topic_prefix, 31);
  wm.addParameter(&p_broker);
  wm.addParameter(&p_port);
  wm.addParameter(&p_prefix);

  // Persist custom params when user hits Save in the portal
  wm.setSaveParamsCallback([&]() {
    strncpy(cfg.mqtt_broker,  p_broker.getValue(), sizeof(cfg.mqtt_broker)  - 1);
    cfg.mqtt_port = atoi(p_port.getValue());
    if (cfg.mqtt_port <= 0) cfg.mqtt_port = 1883;
    strncpy(cfg.topic_prefix, p_prefix.getValue(), sizeof(cfg.topic_prefix) - 1);
    snprintf(MQTT_TOPIC,        sizeof(MQTT_TOPIC),        "%s/gas",    cfg.topic_prefix);
    snprintf(MQTT_STATUS_TOPIC, sizeof(MQTT_STATUS_TOPIC), "%s/status", cfg.topic_prefix);
    saveConfig();
  });

  bool connected = wm.autoConnect("FireWatch-Gas");
  if (!connected) {
    Serial.println("[WiFi] Portal timeout — restarting...");
    ESP.restart();
  }
  Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
}

void connectMQTT() {
  // Use live config — picks up any broker change made via the portal
  mqttClient.setServer(cfg.mqtt_broker, cfg.mqtt_port);

  char willMsg[80];
  snprintf(willMsg, sizeof(willMsg), "{\"node\":\"esp32_gas\",\"status\":\"offline\"}");

  int attempts = 0;
  while (!mqttClient.connected() && attempts < 5) {
    Serial.printf("[MQTT] Connecting to %s:%d as %s...\n",
                  cfg.mqtt_broker, cfg.mqtt_port, MQTT_CLIENT);
    if (mqttClient.connect(MQTT_CLIENT, nullptr, nullptr,
                           MQTT_STATUS_TOPIC, 0, false, willMsg)) {
      Serial.println("[MQTT] Connected!");
      flushQueue();  // publish any payloads buffered while offline
      mqttClient.publish(MQTT_STATUS_TOPIC,
                         "{\"node\":\"esp32_gas\",\"status\":\"online\"}");
    } else {
      Serial.printf("[MQTT] Failed rc=%d — retry in 3s\n", mqttClient.state());
      delay(3000);
      attempts++;
    }
  }

  if (!mqttClient.connected()) {
    Serial.println("[MQTT] All retries exhausted — restarting...");
    delay(1000);
    ESP.restart();
  }
}
