/*
 * ════════════════════════════════════════════════
 *  FireWatch IoT — ESP8266 Temperature Node
 *  Sensor : DHT11 or DHT22 (GPIO14 / D5)
 *  Topic  : <prefix>/temp   (prefix set via config portal)
 *  Broker : configurable — captive portal on first boot
 * ════════════════════════════════════════════════
 *  Libraries (Arduino Library Manager):
 *    - PubSubClient  by Nick O'Leary
 *    - DHT sensor library by Adafruit
 *    - Adafruit Unified Sensor
 *    - WiFiManager   by tzapu/tablatronix
 *    - ArduinoJson   by Benoit Blanchon
 * ════════════════════════════════════════════════
 *  First-boot setup:
 *    1. Power on → AP "FireWatch-Temp" appears
 *    2. Connect phone/laptop to that AP
 *    3. Browser opens portal at 192.168.4.1
 *    4. Enter WiFi SSID/password, MQTT broker host,
 *       port, and topic prefix → click Save
 *    5. Device reboots and connects automatically
 *    6. Config persisted in /fw_config.json on LittleFS
 * ════════════════════════════════════════════════
 */

#include <ArduinoJson.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <WiFiManager.h>

// ── Runtime config (loaded from /fw_config.json on LittleFS) ────────────
struct FWConfig {
  char mqtt_broker[64];
  int  mqtt_port;
  char topic_prefix[32];
};
FWConfig cfg;

// Derived MQTT topics — rebuilt after loadConfig()
char MQTT_TOPIC[48];
char MQTT_STATUS_TOPIC[48];
const char *MQTT_CLIENT = "fw2352_esp8266_temp";

void loadConfig() {
  if (!LittleFS.begin()) {
    Serial.println("[CFG] LittleFS mount failed — using defaults");
  } else {
    File f = LittleFS.open("/fw_config.json", "r");
    if (f) {
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, f)) {
        strlcpy(cfg.mqtt_broker,  doc["broker"] | "broker.hivemq.com", sizeof(cfg.mqtt_broker));
        cfg.mqtt_port = doc["port"] | 1883;
        strlcpy(cfg.topic_prefix, doc["prefix"] | "fw2352",           sizeof(cfg.topic_prefix));
        snprintf(MQTT_TOPIC,        sizeof(MQTT_TOPIC),        "%s/temp",   cfg.topic_prefix);
        snprintf(MQTT_STATUS_TOPIC, sizeof(MQTT_STATUS_TOPIC), "%s/status", cfg.topic_prefix);
        f.close();
        Serial.printf("[CFG] Loaded: broker=%s  port=%d  prefix=%s\n",
                      cfg.mqtt_broker, cfg.mqtt_port, cfg.topic_prefix);
        return;
      }
      f.close();
    }
  }
  // Defaults
  strlcpy(cfg.mqtt_broker,  "broker.hivemq.com", sizeof(cfg.mqtt_broker));
  cfg.mqtt_port = 1883;
  strlcpy(cfg.topic_prefix, "fw2352",            sizeof(cfg.topic_prefix));
  snprintf(MQTT_TOPIC,        sizeof(MQTT_TOPIC),        "%s/temp",   cfg.topic_prefix);
  snprintf(MQTT_STATUS_TOPIC, sizeof(MQTT_STATUS_TOPIC), "%s/status", cfg.topic_prefix);
  Serial.println("[CFG] Using defaults.");
}

void saveConfig() {
  StaticJsonDocument<256> doc;
  doc["broker"] = cfg.mqtt_broker;
  doc["port"]   = cfg.mqtt_port;
  doc["prefix"] = cfg.topic_prefix;
  File f = LittleFS.open("/fw_config.json", "w");
  if (f) { serializeJson(doc, f); f.close(); Serial.println("[CFG] Saved to LittleFS."); }
  else   { Serial.println("[CFG] ERROR: could not write /fw_config.json"); }
}

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

// ── Offline queue: buffer up to 10 payloads when broker is unreachable ─────
#define OFFLINE_QUEUE_SIZE 10
String offlineQueue[OFFLINE_QUEUE_SIZE];
int    oqHead  = 0;
int    oqTail  = 0;
int    oqCount = 0;

void queuePayload(const char *payload) {
  offlineQueue[oqTail] = payload;
  oqTail = (oqTail + 1) % OFFLINE_QUEUE_SIZE;
  if (oqCount < OFFLINE_QUEUE_SIZE) {
    oqCount++;
  } else {
    oqHead = (oqHead + 1) % OFFLINE_QUEUE_SIZE;
    Serial.println("[Q] Buffer full — oldest entry overwritten");
  }
  Serial.printf("[Q] Queued (#%d): %s\n", oqCount, payload);
}

void flushQueue();

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

  // Mount LittleFS and load saved config (or write defaults on first boot)
  loadConfig();
  // connectWiFi() launches WiFiManager portal if no credentials are stored
  connectWiFi();

  mqttClient.setKeepAlive(60);

  connectMQTT();
  Serial.println("[FireWatch] Temperature node ready.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected — reconnecting...");
    WiFi.reconnect();
    unsigned long _t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - _t < 10000) delay(200);
  }
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

  if (mqttClient.connected()) {
    if (mqttClient.publish(MQTT_TOPIC, payload, false)) {
      Serial.printf("[TEMP] Published → %s : %s\n", MQTT_TOPIC, payload);
    } else {
      Serial.println("[TEMP] Publish FAILED — queuing");
      queuePayload(payload);
    }
  } else {
    queuePayload(payload);
  }
}

// ── Offline queue flush ──────────────────────────────────────────────────
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
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

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

  wm.setSaveParamsCallback([&]() {
    strlcpy(cfg.mqtt_broker,  p_broker.getValue(), sizeof(cfg.mqtt_broker)  - 1);
    cfg.mqtt_port = atoi(p_port.getValue());
    if (cfg.mqtt_port <= 0) cfg.mqtt_port = 1883;
    strlcpy(cfg.topic_prefix, p_prefix.getValue(), sizeof(cfg.topic_prefix) - 1);
    snprintf(MQTT_TOPIC,        sizeof(MQTT_TOPIC),        "%s/temp",   cfg.topic_prefix);
    snprintf(MQTT_STATUS_TOPIC, sizeof(MQTT_STATUS_TOPIC), "%s/status", cfg.topic_prefix);
    saveConfig();
  });

  bool connected = wm.autoConnect("FireWatch-Temp");
  if (!connected) {
    Serial.println("[WiFi] Portal timeout — restarting...");
    ESP.restart();
  }
  Serial.printf("[WiFi] Connected! IP: %s  RSSI: %ddBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

void connectMQTT() {
  // Use live config — picks up any broker change made via the portal
  mqttClient.setServer(cfg.mqtt_broker, cfg.mqtt_port);

  char willMsg[80];
  snprintf(willMsg, sizeof(willMsg), "{\"node\":\"esp8266_temp\",\"status\":\"offline\"}");

  int attempts = 0;
  while (!mqttClient.connected() && attempts < 5) {
    Serial.printf("[MQTT] Connecting to %s:%d as %s...\n",
                  cfg.mqtt_broker, cfg.mqtt_port, MQTT_CLIENT);
    if (mqttClient.connect(MQTT_CLIENT, nullptr, nullptr,
                           MQTT_STATUS_TOPIC, 0, false, willMsg)) {
      Serial.println("[MQTT] Connected!");
      flushQueue();  // publish any payloads buffered while offline
      mqttClient.publish(MQTT_STATUS_TOPIC,
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
