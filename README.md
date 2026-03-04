# 🔥 FireWatch IoT — Real-Time Fire Safety Monitoring System

## Full Setup Guide

---

## System Architecture

```
ESP32  (MQ-6 Gas Sensor)      ─── edge: ring-buffer MA, ROC, delta-publish ──┐
ESP8266 (DHT11 Temp Sensor)   ─── edge: EMA, heat index, ROC, delta-publish ──┼──► [configurable broker] ──► Node-RED
Pico W  (PIR HC-SR501)        ─── edge: freq window, activity, occupancy ─────┘                              HTML Dashboard
```

Each node performs **on-device edge computation** before publishing to MQTT:

- Smoothing (ring-buffer MA / EMA) to remove sensor noise
- Rate-of-change detection for early fire warning
- Delta-based conditional publishing to reduce MQTT traffic (~70–80% fewer messages at idle)
- Per-sensor `risk` score (0–100), `trend`, and derived metrics in every payload

---

## File Structure

```
Firewatch_IOT/
├── nodered_flow.json              ← Import into Node-RED
├── firewatch_dashboard.html       ← Standalone browser dashboard (MQTT over WSS)
├── esp32_gas_node.ino             ← Arduino IDE — ESP32
├── esp8266_temp_node.ino          ← Arduino IDE — ESP8266
└── pico_w_motion_main.py          ← MicroPython — Raspberry Pi Pico W
```

---

## Network-Agnostic Configuration

**No WiFi credentials or broker addresses are hardcoded.** Each device discovers its configuration at runtime:

| Device | How config is stored | How to change |
|--------|---------------------|---------------|
| ESP32 | NVS (Preferences, flash) | Re-open captive portal by erasing WiFi creds |
| ESP8266 | `/fw_config.json` (LittleFS) | Re-open captive portal or reflash with new file |
| Pico W | `/config.json` (flash) | Edit via `mpremote fs cp` |
| Dashboard | `localStorage` | ⚙ Connection Config panel in the UI |
| Node-RED | Environment variables | Set `MQTT_BROKER_HOST` / `MQTT_BROKER_PORT` |

### Arduino Captive Portal (ESP32 + ESP8266)

On **first boot** (or after erasing stored credentials), the device broadcasts a WiFi access point:

| Device | AP Name | Portal address |
|--------|---------|----------------|
| ESP32  | `FireWatch-Gas`  | `192.168.4.1` |
| ESP8266 | `FireWatch-Temp` | `192.168.4.1` |

1. Connect your phone or laptop to the AP
2. A captive portal page opens automatically (or navigate to `192.168.4.1`)
3. Enter: **WiFi SSID**, **WiFi Password**, **MQTT Broker Host**, **MQTT Port**, **Topic Prefix**
4. Click **Save** — the device reboots and connects

Config is persisted across power cycles (NVS on ESP32, LittleFS JSON on ESP8266).  
The portal re-opens after 3 minutes of inactivity if not submitted.

### Pico W — config.json

`/config.json` is created automatically with defaults on first boot:

```json
{
  "wifi_ssid":    "your_wifi_ssid",
  "wifi_pass":    "your_wifi_password",
  "mqtt_broker":  "broker.hivemq.com",
  "mqtt_port":    1883,
  "topic_prefix": "fw2352"
}
```

Edit on your PC and upload:

```powershell
# 1. Edit config.json locally (it's in the project root)
# 2. Upload to Pico W flash
mpremote connect COM11 fs cp config.json :config.json

# 3. Hard-reset to apply
mpremote connect COM11 reset
```

To read the live config from the device:

```powershell
mpremote connect COM11 exec "import ujson; print(open('/config.json').read())"
```

### Dashboard — Connection Config Panel

Click **🔌 CONNECTION CONFIG** (below the Telegram panel) to expand the settings panel:

| Field | Default | Notes |
|-------|---------|-------|
| Broker Host / IP | `broker.hivemq.com` | Use `192.168.x.x` for local Mosquitto |
| WSS Port | `8884` | WebSocket port exposed by broker (local: `9001`) |
| TCP Port | `1883` | Shown as reference for firmware devices |
| Topic Prefix | `fw2352` | Must match the prefix set on firmware devices |

Click **💾 SAVE & RECONNECT** — settings persist in `localStorage` and the dashboard reconnects immediately.

### Node-RED — Environment Variables

Set these **before** starting Node-RED so the flow connects to the correct broker:

```powershell
# Windows (PowerShell) — public HiveMQ
$env:MQTT_BROKER_HOST = "broker.hivemq.com"
$env:MQTT_BROKER_PORT = "1883"
node-red

# Windows — local Mosquitto
$env:MQTT_BROKER_HOST = "127.0.0.1"
$env:MQTT_BROKER_PORT = "1883"
node-red
```

```bash
# Linux/macOS
MQTT_BROKER_HOST=broker.hivemq.com MQTT_BROKER_PORT=1883 node-red
```

Or add permanently to Node-RED's `settings.js`:

```js
process.env.MQTT_BROKER_HOST = 'broker.hivemq.com';
process.env.MQTT_BROKER_PORT = '1883';
```

### Offline Queue (all firmware nodes)

All three nodes buffer up to **10 MQTT payloads** in memory when the broker is unreachable.  
The queue is flushed automatically on the next successful reconnect.

```
[Q] Queued (#1): {"value":487,"raw":2105,...}
...
[Q] Flushing 3 buffered payload(s)...
[Q] Flushed 3/3 payload(s).
```

If the buffer fills (more than 10 messages while offline), the oldest entry is overwritten — most-recent sensor data is always preserved.

---

## MQTT Topics

Topics use the configured prefix (default `fw2352`):

| Topic              | Publisher | QoS | Payload (after edge computation)                                                                          |
|--------------------|-----------|-----|----------------------------------------------------------------------------------------------------------|
| `{prefix}/gas`     | ESP32     | 0   | `{"value":487,"raw":2105,"status":"warning","trend":"rising","alert":false,"risk":48}`                   |
| `{prefix}/temp`    | ESP8266   | 0   | `{"temperature":36.2,"humidity":61.0,"heat_index":38.5,"status":"elevated","trend":"rising","alert":false,"risk":31}` |
| `{prefix}/motion`  | Pico W    | 0   | `{"motion":true,"count":5,"status":"detected","freq":3,"activity":"moderate","occupied":true,"risk":60}` |
| `{prefix}/status`  | All nodes | 0   | `{"node":"esp32_gas","status":"online"}`                                                                 |
| `{prefix}/cmd/motion` | Dashboard | 0 | `"arm"` or `"disarm"`                                                                                  |

**Legacy / test payloads also accepted** by dashboard and Node-RED:

- `{prefix}/gas` → `450` or `"leak"` or `"safe"`
- `{prefix}/temp` → `38.5` or `"65"`
- `{prefix}/motion` → `1` or `0` or `"detected"` or `"clear"`

---

## Hardware Wiring

### ESP32 — Gas Detection

| Component      | ESP32 Pin            |
|----------------|----------------------|
| MQ-6 VCC       | 3.3 V or 5 V         |
| MQ-6 GND       | GND                  |
| MQ-6 A0        | GPIO34 (ADC1_CH6)    |
| Buzzer (+)     | GPIO25               |
| Buzzer (−)     | GND                  |
| Onboard LED    | GPIO2 (built-in)     |

> **Edge thresholds (delta above auto-calibrated baseline):**
> safe → baseline+65 = warning → baseline+105 = leak alert

### ESP8266 — Temperature

| Component      | NodeMCU Pin           |
|----------------|-----------------------|
| DHT11 VCC      | 3.3 V                 |
| DHT11 GND      | GND                   |
| DHT11 DATA     | D5 (GPIO14)           |
| 10 kΩ pull-up  | DATA → 3.3 V          |
| Buzzer (+)     | D1 (GPIO5)            |
| Onboard LED    | D4 (GPIO2, active LOW)|

> **Edge thresholds (EMA-smoothed, heat-index-aware):**
> normal < 30 °C → elevated ≥ 30 °C → high ≥ 38 °C → critical ≥ 45 °C

### Raspberry Pi Pico W — Motion

| Component       | Pico W Pin           |
|-----------------|----------------------|
| HC-SR501 VCC    | VBUS (5 V)           |
| HC-SR501 GND    | GND                  |
| HC-SR501 OUT    | GP15                 |
| External LED    | GP14 (via 220 Ω)     |
| Onboard LED     | LED (built-in)       |

> HC-SR501 has two onboard trimmers: sensitivity (CW = more sensitive) and retrigger hold time.

---

## 1 · Raspberry Pi Pico W Setup

### Step 1 — Flash MicroPython firmware

1. **Hold** BOOTSEL, plug USB in, release — board mounts as **RPI-RP2**
2. Download Pico W MicroPython `.uf2`: <https://micropython.org/download/RPI_PICO_W/>
3. Drag `.uf2` onto **RPI-RP2** — board reboots into MicroPython

### Step 2 — Install mpremote

```powershell
& "c:\Users\srikr\Desktop\Studies\Sem 6\CNIC\Firewatch_IOT\.venv\Scripts\Activate.ps1"
pip install mpremote
```

### Step 3 — Find the Pico W COM port

```powershell
mpremote connect list     # before plug-in
# plug in Pico W
mpremote connect list     # new entry = Pico W port
```

Example: `COM11  MicroPython Board  2E8A:0005`

### Step 4 — Install umqtt.simple (one-time)

```powershell
mpremote connect COM11 mip install umqtt.simple
```

### Step 5 — Create and upload config.json

Create `config.json` in the project root:

```json
{
  "wifi_ssid":    "YourNetworkName",
  "wifi_pass":    "YourPassword",
  "mqtt_broker":  "broker.hivemq.com",
  "mqtt_port":    1883,
  "topic_prefix": "fw2352"
}
```

Upload to Pico W:

```powershell
mpremote connect COM11 fs cp config.json :config.json
```

### Step 6 — Deploy the script

```powershell
# Copy + reset in one command
mpremote connect COM11 cp pico_w_motion_main.py :main.py + reset
```

### Step 7 — Monitor serial output

```powershell
mpremote connect COM11 repl
```

Press **Ctrl-C** to interrupt · **Ctrl-X** or **Ctrl-]** to exit mpremote.

### Re-deploy after edits

```powershell
mpremote connect COM11 cp pico_w_motion_main.py :main.py + reset
```

### Test run without saving to flash

```powershell
mpremote connect COM11 run pico_w_motion_main.py
```

### Arm / Disarm motion detection

```powershell
mosquitto_pub -h broker.hivemq.com -t "fw2352/cmd/motion" -m "disarm"
mosquitto_pub -h broker.hivemq.com -t "fw2352/cmd/motion" -m "arm"
```

---

## 2 · Arduino IDE Setup (ESP32 + ESP8266)

### Board manager URLs

| Board   | URL |
|---------|-----|
| ESP32   | `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json` |
| ESP8266 | `http://arduino.esp8266.com/stable/package_esp8266com_index.json` |

Add via **File → Preferences → Additional Boards Manager URLs**, then install each board package.

### Required libraries (Library Manager)

| Library | Purpose | Boards |
|---------|---------|--------|
| `PubSubClient` by Nick O'Leary | MQTT client | Both |
| `WiFiManager` by tzapu | Captive portal WiFi config | Both |
| `ArduinoJson` by Benoit Blanchon | Config file parsing | ESP8266 |
| `DHT sensor library` by Adafruit | DHT11/22 | ESP8266 |
| `Adafruit Unified Sensor` | Dependency for DHT | ESP8266 |

### Flash settings

| Board | Setting | Value |
|-------|---------|-------|
| ESP32 | Board | `ESP32 Dev Module` |
| ESP32 | Upload Speed | `921600` |
| ESP8266 | Board | `NodeMCU 1.0 (ESP-12E Module)` |
| ESP8266 | Upload Speed | `115200` |
| Both | Port | COM port that appears when plugged in |

### First-boot captive portal walkthrough

1. Flash the `.ino` sketch via Arduino IDE
2. Open Serial Monitor at **115200 baud** — you will see:
   ```
   [WiFi] No saved credentials — starting config portal...
   ```
3. On your phone: connect to **`FireWatch-Gas`** (ESP32) or **`FireWatch-Temp`** (ESP8266)
4. Browser auto-opens the portal — fill in all fields and click **Save**
5. Device reboots and you see `[WiFi] Connected! IP: 192.168.x.x`

To re-run the portal (e.g. to change broker), erase flash from Arduino IDE:  
**Tools → Erase Flash → All Flash Contents**, then re-upload.

---

## 3 · Node-RED Setup

### Install

```bash
npm install -g --unsafe-perm node-red
cd ~/.node-red       # Windows: %USERPROFILE%\.node-red
npm install node-red-dashboard
```

### Set broker env vars, then start

```powershell
# Windows PowerShell
$env:MQTT_BROKER_HOST = "broker.hivemq.com"
$env:MQTT_BROKER_PORT = "1883"
node-red
```

```bash
# Linux / macOS
MQTT_BROKER_HOST=broker.hivemq.com MQTT_BROKER_PORT=1883 node-red
```

### Import the flow

1. Open <http://localhost:1880>
2. **☰ Menu → Import → select file** → `nodered_flow.json` → **Import**
3. Click **Deploy**
4. Dashboard: <http://localhost:1880/ui>

### What the Node-RED flow does

| Function node | Reads | Stores in flow context |
|---------------|-------|----------------------|
| Process Gas | `value`, `trend`, `risk`, `alert` | `gas_value`, `gas_trend`, `gas_risk` |
| Process Temperature | `temperature`, `heat_index`, `trend`, `risk` | `temp_value`, `temp_heat_index`, `temp_risk` |
| Process Motion | `motion`, `count`, `freq`, `activity`, `occupied`, `risk` | `motion_count`, `motion_activity`, `motion_occupied`, `motion_risk` |
| Combine Alert | all `*_alert` + `*_risk` | Combined risk banner |

---

## 4 · Standalone HTML Dashboard

Open `firewatch_dashboard.html` in any browser — no server required.

Connects to the broker via **WebSocket (WSS)**. HiveMQ public exposes port `8884`; local Mosquitto typically uses `9001`.

### Configuring the broker

1. Click **🔌 CONNECTION CONFIG** (collapsible panel below Telegram settings)
2. Enter broker host, WSS port, and topic prefix
3. Click **💾 SAVE & RECONNECT**

Settings persist in `localStorage` — survive page refreshes.

### Edge-computation fields displayed

| Card | Edge fields shown |
|------|------------------|
| Gas | TREND (rising/stable) · RISK/100 |
| Temperature | HEAT IDX (°C) · TREND · RISK/100 |
| Motion | FREQ/min · ACTIVITY (idle/low/moderate/high) · OCCUPIED · RISK/100 |

---

## Alert Thresholds Summary

### Gas (ESP32 — delta above auto-calibrated baseline)

| Level | Condition |
|-------|-----------|
| safe | delta < 65 |
| warning | delta ≥ 65 **or** trend = rising |
| leak | delta ≥ 105 |

### Temperature (ESP8266 — EMA-smoothed, heat-index-aware)

| Level | Condition |
|-------|-----------|
| normal | EMA < 30 °C |
| elevated | EMA ≥ 30 °C |
| high | heat_index ≥ 38 °C |
| critical | heat_index ≥ 45 °C |

### Motion (Pico W — frequency-based)

| Activity | Events in last 60 s |
|----------|---------------------|
| idle | 0 |
| low | 1–2 |
| moderate | 3–5 |
| high | > 5 |

Occupancy: last detection within **5 minutes** → `occupied: true`

---

## Testing with MQTT CLI

```bash
# Install: https://mosquitto.org/download/
# Linux: sudo apt install mosquitto-clients
# macOS: brew install mosquitto

# --- Full edge-payload test ---
mosquitto_pub -h broker.hivemq.com -t "fw2352/gas" \
  -m '{"value":512,"raw":2200,"status":"warning","trend":"rising","alert":false,"risk":50}'

mosquitto_pub -h broker.hivemq.com -t "fw2352/temp" \
  -m '{"temperature":39.5,"humidity":65.0,"heat_index":42.1,"status":"high","trend":"stable","alert":true,"risk":37}'

mosquitto_pub -h broker.hivemq.com -t "fw2352/motion" \
  -m '{"motion":true,"count":4,"status":"detected","freq":4,"activity":"moderate","occupied":true,"risk":80}'

# --- Simple scalar payloads ---
mosquitto_pub -h broker.hivemq.com -t "fw2352/gas"    -m "850"
mosquitto_pub -h broker.hivemq.com -t "fw2352/temp"   -m "65"
mosquitto_pub -h broker.hivemq.com -t "fw2352/motion" -m "1"

# --- Subscribe to everything ---
mosquitto_sub -h broker.hivemq.com -t "fw2352/#" -v

# --- Arm / disarm Pico W ---
mosquitto_pub -h broker.hivemq.com -t "fw2352/cmd/motion" -m "disarm"
mosquitto_pub -h broker.hivemq.com -t "fw2352/cmd/motion" -m "arm"
```

For a **local Mosquitto** broker, replace `-h broker.hivemq.com` with `-h localhost`.

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| **ESP32/8266 portal never appears** | Erase flash (Tools → Erase Flash → All Flash Contents) then re-upload |
| **Portal appears but doesn't save** | Ensure MQTT port field is numeric (default 1883); portal times out after 3 min |
| **Device connects to WiFi but not MQTT** | Check broker host is reachable on port 1883 from the device's network; try `127.0.0.1` + local Mosquitto |
| **Pico W ignores new config.json** | Hard-reset after upload: `mpremote connect COMxx reset` |
| **Pico W WiFi status = -1** | Hotspot must be **2.4 GHz** — Pico W does not support 5 GHz |
| `mpremote: no device found` | Run `mpremote connect list` before/after plug-in to identify the port |
| Pico W mounts as RPI-RP2 drive | MicroPython not flashed yet — drag the `.uf2` onto the drive |
| `mpremote` not found | Activate the venv: `& ".venv\Scripts\Activate.ps1"` |
| `umqtt.simple` import error | `mpremote connect COMxx mip install umqtt.simple` |
| **Dashboard shows no data** | Check Connection Config panel — broker host and WSS port must match |
| **Node-RED can't connect to broker** | Verify env vars are exported before `node-red` starts; check broker host/port |
| Dashboard shows wrong topics | Topic prefix in Connection Config panel must match prefix on the firmware |
| Offline queue logged but never flushed | Broker unreachable — check network; queue auto-flushes on next successful connect |
| ESP32 ADC noise | Add 100 nF cap between GPIO34 and GND; baseline auto-calibration handles drift |
| DHT11 read fails | Increase warm-up delay to 5 s; verify 10 kΩ pull-up on data line |
| PIR false triggers | Adjust onboard sensitivity trimmer (CCW = less sensitive); debounce = 500 ms |
| Node-RED `ui_led` missing | `cd ~/.node-red && npm install node-red-dashboard` |
| Heat index not showing | Only computed above 27 °C and ≥ 40 % RH — normal at room temperature |

---

## Security Notes

> ⚠️ Default broker is **broker.hivemq.com** (public, unauthenticated).  
> All config fields (broker host, port, prefix) can be changed at runtime without reflashing.  
> For production: use a **private MQTT broker** with TLS and client certificates.  
> Recommended options: EMQX Cloud, AWS IoT Core, or self-hosted Mosquitto with TLS.

---

*FireWatch IoT — CNIC Semester 6 End Semester Project*
