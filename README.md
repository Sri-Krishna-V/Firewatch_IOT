# 🔥 FireWatch IoT — Real-Time Fire Safety Monitoring System

## Full Setup Guide

---

## System Architecture

```
ESP32  (MQ-2/5 Gas Sensor)    ─── edge: ring-buffer MA, ROC, delta-publish ──┐
ESP8266 (DHT11 Temp Sensor)   ─── edge: EMA, heat index, ROC, delta-publish ──┼──► broker.hivemq.com:1883 ──► Node-RED
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

## MQTT Topics

| Topic            | Publisher | QoS | Payload (after edge computation)                                                                 |
|------------------|-----------|-----|--------------------------------------------------------------------------------------------------|
| `fw2352/gas`     | ESP32     | 0   | `{"value":487,"raw":2105,"status":"warning","trend":"rising","alert":false,"risk":48}`           |
| `fw2352/temp`    | ESP8266   | 0   | `{"temperature":36.2,"humidity":61.0,"heat_index":38.5,"status":"elevated","trend":"rising","alert":false,"risk":31}` |
| `fw2352/motion`  | Pico W    | 0   | `{"motion":true,"count":5,"status":"detected","freq":3,"activity":"moderate","occupied":true,"risk":60}` |
| `fw2352/status`  | All nodes | 0   | `{"node":"esp32_gas","status":"online"}`                                                         |
| `fw2352/cmd/motion` | Dashboard | 0 | `"arm"` or `"disarm"`                                                                           |

**Legacy / test payloads also accepted** by the dashboard and Node-RED:

- `fw2352/gas` → `450` or `"leak"` or `"safe"`
- `fw2352/temp` → `38.5` or `"65"`
- `fw2352/motion` → `1` or `0` or `"detected"` or `"clear"`

---

## Hardware Wiring

### ESP32 — Gas Detection

| Component      | ESP32 Pin            |
|----------------|----------------------|
| MQ-2/5 VCC     | 3.3 V or 5 V         |
| MQ-2/5 GND     | GND                  |
| MQ-2/5 A0      | GPIO34 (ADC1_CH6)    |
| Buzzer (+)     | GPIO25               |
| Buzzer (−)     | GND                  |
| Onboard LED    | GPIO2 (built-in)     |

> **Edge thresholds (delta above auto-calibrated baseline):**
> safe → baseline+65 = warning → baseline+105 = leak alert

### ESP8266 — Temperature

| Component      | NodeMCU Pin          |
|----------------|----------------------|
| DHT11 VCC      | 3.3 V                |
| DHT11 GND      | GND                  |
| DHT11 DATA     | D5 (GPIO14)          |
| 10 kΩ pull-up  | DATA → 3.3 V         |
| Buzzer (+)     | D1 (GPIO5)           |
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

1. **Hold** the BOOTSEL button on the Pico W, then plug the USB cable in, then release.  
   The board mounts as a USB drive called **RPI-RP2**.
2. Download the latest Pico W MicroPython `.uf2`:  
   <https://micropython.org/download/RPI_PICO_W/>
3. Drag the `.uf2` file onto the **RPI-RP2** drive. The board reboots automatically into MicroPython.

### Step 2 — Install mpremote (one-time, into the project venv)

```powershell
# From the project root — activate the venv first
& "c:\Users\srikr\Desktop\Studies\Sem 6\CNIC\Firewatch_IOT\.venv\Scripts\Activate.ps1"

pip install mpremote
```

### Step 3 — Find the Pico W COM port

Run this **before** and **after** plugging in the Pico W — the new entry is the Pico:

```powershell
mpremote connect list
```

Example output when Pico W is detected:
```
COM11  MicroPython Board  2E8A:0005  MicroPython  Board in FS mode
```

> If the Pico still shows as **RPI-RP2** (mass-storage), MicroPython has not been flashed yet — repeat Step 1.

### Step 4 — Install umqtt.simple on the Pico W (one-time)

```powershell
# Replace COM11 with your actual port
mpremote connect COM11 mip install umqtt.simple
```

### Step 5 — Deploy the script

```powershell
# Copy script to Pico W flash as main.py (runs on every boot)
mpremote connect COM11 cp pico_w_motion_main.py :main.py

# Soft-reset to start
mpremote connect COM11 reset
```

### Step 6 — Monitor serial output

```powershell
mpremote connect COM11 repl
```

Press **Ctrl-C** inside REPL to interrupt the running script.  
Press **Ctrl-X** or **Ctrl-]** to exit mpremote.

### Re-deploy after edits

```powershell
# Copy + immediate reset in one command
mpremote connect COM11 cp pico_w_motion_main.py :main.py + reset
```

### Test run without writing to flash

```powershell
# Runs script directly without saving — useful during development
mpremote connect COM11 run pico_w_motion_main.py
```

### Arm / Disarm motion detection via MQTT

```powershell
# Disarm — Pico W stops publishing motion events
mosquitto_pub -h broker.hivemq.com -t "fw2352/cmd/motion" -m "disarm"

# Re-arm
mosquitto_pub -h broker.hivemq.com -t "fw2352/cmd/motion" -m "arm"
```

---

## 2 · Arduino IDE Setup (ESP32 + ESP8266)

### Board manager URLs

| Board   | URL |
|---------|-----|
| ESP32   | `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json` |
| ESP8266 | `http://arduino.esp8266.com/stable/package_esp8266com_index.json` |

Add via: **File → Preferences → Additional Boards Manager URLs**

Then install:
- **ESP32** — search "esp32" in Boards Manager, install by Espressif (v3.x required for LEDC API)
- **ESP8266** — search "esp8266", install by ESP8266 Community

### Required libraries (Library Manager)

| Library | Install name |
|---------|-------------|
| MQTT client | `PubSubClient` by Nick O'Leary |
| DHT sensor (ESP8266 only) | `DHT sensor library` by Adafruit |
| Adafruit Unified Sensor | `Adafruit Unified Sensor` by Adafruit |

### Flash settings

| Board | Setting | Value |
|-------|---------|-------|
| ESP32 | Board | `ESP32 Dev Module` |
| ESP32 | Upload Speed | `921600` |
| ESP8266 | Board | `NodeMCU 1.0 (ESP-12E Module)` |
| ESP8266 | Upload Speed | `115200` |
| Both | Port | Whichever COM port appears when plugged in |

---

## 3 · Node-RED Setup

### Install

```bash
# Requires Node.js 18+
npm install -g --unsafe-perm node-red

# Install dashboard palette
cd ~/.node-red       # Windows: %USERPROFILE%\.node-red
npm install node-red-dashboard

# Start Node-RED
node-red
```

### Import the flow

1. Open <http://localhost:1880>
2. **☰ Menu → Import → select file** → choose `nodered_flow.json` → **Import**
3. Click **Deploy** (top-right red button)
4. Dashboard: <http://localhost:1880/ui>

### What the Node-RED flow does

Each MQTT input feeds a function node that reads the edge-computed fields:

| Function node | Reads from payload | Stores in flow context |
|---------------|-------------------|------------------------|
| Process Gas | `value`, `trend`, `risk`, `alert` | `gas_value`, `gas_trend`, `gas_risk`, `gas_alert` |
| Process Temperature | `temperature`, `heat_index`, `trend`, `risk` | `temp_value`, `temp_heat_index`, `temp_trend`, `temp_risk`, `temp_alert` |
| Process Motion | `motion`, `count`, `freq`, `activity`, `occupied`, `risk` | `motion_count`, `motion_activity`, `motion_occupied`, `motion_risk`, `motion_alert` |
| Combine Alert Status | all `*_alert` + `*_risk` flags | — |

The combined alert banner format:
```
⚠ ALERT: [GAS] WARNING: Elevated Gas (risk 48) | [TEMP] HIGH TEMP WARNING (risk 72)  |  COMBINED RISK: 72/100
```

---

## 4 · Standalone HTML Dashboard

Open `firewatch_dashboard.html` directly in any browser — no server required.

Connects to HiveMQ via **WebSocket (WSS port 8884)**.

### New edge-computation fields displayed

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
| safe | delta < 65  |
| warning | delta ≥ 65 **or** trend = rising |
| leak | delta ≥ 105 |

### Temperature (ESP8266 — EMA-smoothed, heat-index-aware)

| Level | Condition |
|-------|-----------|
| normal | EMA < 30 °C |
| elevated | EMA ≥ 30 °C **or** trend = rising |
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
# Windows: install from https://mosquitto.org/download/
# Linux/macOS:
sudo apt install mosquitto-clients   # Debian/Ubuntu
brew install mosquitto               # macOS

# --- Publish test payloads (new format with edge fields) ---
mosquitto_pub -h broker.hivemq.com -t "fw2352/gas" \
  -m '{"value":512,"raw":2200,"status":"warning","trend":"rising","alert":false,"risk":50}'

mosquitto_pub -h broker.hivemq.com -t "fw2352/temp" \
  -m '{"temperature":39.5,"humidity":65.0,"heat_index":42.1,"status":"high","trend":"stable","alert":true,"risk":37}'

mosquitto_pub -h broker.hivemq.com -t "fw2352/motion" \
  -m '{"motion":true,"count":4,"status":"detected","freq":4,"activity":"moderate","occupied":true,"risk":80}'

# --- Simple scalar payloads (also accepted) ---
mosquitto_pub -h broker.hivemq.com -t "fw2352/gas"    -m "850"
mosquitto_pub -h broker.hivemq.com -t "fw2352/temp"   -m "65"
mosquitto_pub -h broker.hivemq.com -t "fw2352/motion" -m "1"

# --- Subscribe to everything ---
mosquitto_sub -h broker.hivemq.com -t "fw2352/#" -v

# --- Arm / disarm Pico W remotely ---
mosquitto_pub -h broker.hivemq.com -t "fw2352/cmd/motion" -m "disarm"
mosquitto_pub -h broker.hivemq.com -t "fw2352/cmd/motion" -m "arm"
```

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| `mpremote: no device found` | Pico W not plugged in, or MicroPython not flashed. Run `mpremote connect list` before and after plugging in to identify the port, then use `mpremote connect COMxx …` explicitly. |
| Pico W mounts as RPI-RP2 drive | MicroPython not flashed yet — drag the `.uf2` onto the drive. |
| `mpremote` not found | Activate the venv first: `& ".venv\Scripts\Activate.ps1"` |
| `umqtt.simple` import error | Run `mpremote connect COMxx mip install umqtt.simple` |
| MQTT not connecting (Arduino) | Check port 1883 not blocked; device restarts automatically after 5 failed attempts |
| ESP32 ADC noise | Add 100 nF capacitor between GPIO34 and GND; baseline auto-calibration handles most drift |
| DHT11 read fails | Increase warm-up delay to 5 s; verify 10 kΩ pull-up on data line |
| PIR false triggers | Adjust onboard sensitivity trimmer (CCW = less sensitive); `DEBOUNCE_MS = 500` already set |
| Node-RED `ui_led` missing | `cd ~/.node-red && npm install node-red-dashboard` |
| Dashboard shows no data | Check <https://status.hivemq.com>; try refreshing — dashboard auto-reconnects |
| Heat index not showing | Only computed above 27 °C and ≥ 40 % RH — normal at room temperature |

---

## Security Notes

> ⚠️ This project uses **broker.hivemq.com** (public, unauthenticated).  
> For production deployment, use a **private MQTT broker** with TLS and client certificates.  
> Recommended options: EMQX Cloud, AWS IoT Core, or self-hosted Mosquitto with TLS.

---

*FireWatch IoT — CNIC Semester 6 End Semester Project*
