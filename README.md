# 🔥 FireWatch IoT — Real-Time Fire Safety Monitoring System

## Full Setup Guide

---

## System Architecture

```
ESP32 (MQ-2 Gas Sensor)       ──┐
ESP8266 (DHT11 Temp Sensor)   ──┼──► broker.hivemq.com:1883 ──► Node-RED Dashboard
Raspberry Pi Pico W (PIR)     ──┘                              Standalone HTML Dashboard
```

---

## File Structure

```
firewatch/
├── nodered_flow.json              ← Import into Node-RED
├── firewatch_dashboard.html       ← Standalone browser dashboard (MQTT over WS)
├── esp32_gas_node/
│   └── esp32_gas_node.ino         ← Arduino IDE — ESP32
├── esp8266_temp_node/
│   └── esp8266_temp_node.ino      ← Arduino IDE — ESP8266
└── pico_motion_node/
    └── main.py                    ← MicroPython — Raspberry Pi Pico W
```

---

## MQTT Topics

| Topic       | Publisher    | Payload Example                                         |
|-------------|-------------|----------------------------------------------------------|
| fire/gas    | ESP32        | `{"value":450,"status":"leak","alert":true}`             |
| fire/temp   | ESP8266      | `{"temperature":47.5,"humidity":62.0,"status":"high","alert":true}` |
| fire/motion | Pico W       | `{"motion":true,"count":3,"status":"detected"}`         |
| fire/status | All nodes    | `{"node":"esp32_gas","status":"online"}`                 |

**Simple payloads also supported** (for testing):

- `fire/gas` → `450` or `"leak"` or `"safe"`
- `fire/temp` → `38.5` or `"65"`
- `fire/motion` → `1` or `0` or `"detected"` or `"clear"`

---

## Hardware Wiring

### ESP32 — Gas Detection

| Component | ESP32 Pin |
|-----------|-----------|
| MQ-2/5 VCC | 3.3V or 5V |
| MQ-2/5 GND | GND |
| MQ-2/5 A0  | GPIO34 (ADC) |
| Buzzer (+) | GPIO25 |
| Buzzer (-) | GND |

> ADC Thresholds: >400 = ALERT, 200–400 = WARNING, <200 = SAFE

### ESP8266 — Temperature

| Component | NodeMCU Pin |
|-----------|-------------|
| DHT11 VCC | 3.3V |
| DHT11 GND | GND |
| DHT11 DATA | D2 (GPIO4) |
| 10kΩ pull-up | DATA to VCC |

> Temp Thresholds: ≥60°C = CRITICAL, ≥45°C = HIGH, ≥35°C = ELEVATED

### Raspberry Pi Pico W — Motion

| Component | Pico W Pin |
|-----------|-----------|
| HC-SR501 VCC | VBUS (5V) |
| HC-SR501 GND | GND |
| HC-SR501 OUT | GP15 |

> HC-SR501 has onboard potentiometers for sensitivity and hold time

---

## Node-RED Setup

### Prerequisites

```bash
# Install Node-RED
npm install -g --unsafe-perm node-red

# Install required palettes
cd ~/.node-red
npm install node-red-dashboard
npm install node-red-contrib-mqtt-broker   # optional local broker

# Start Node-RED
node-red
```

### Import the Flow

1. Open Node-RED: `http://localhost:1880`
2. Menu (☰) → **Import** → paste `nodered_flow.json` content → **Import**
3. Click **Deploy** (red button)
4. Open dashboard: `http://localhost:1880/ui`

### Dashboard URL

```
http://localhost:1880/ui
```

---

## Standalone HTML Dashboard

Open `firewatch_dashboard.html` directly in any browser.

**Features:**

- Connects to HiveMQ via **WebSocket** (WSS port 8884) — no server needed
- Animated semicircle gauges with needle
- Color-coded LEDs: 🟢 SAFE → 🟡 WARNING → 🔴 DANGER
- Radar animation for motion detection
- Live event log with timestamps
- Temperature sparkline chart
- Session statistics
- **Auto-demo mode** if MQTT unavailable (cycles through test data)

---

## Arduino IDE Setup

### ESP32 Board Manager

1. File → Preferences → Additional Board URLs:

   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```

2. Tools → Board Manager → search "esp32" → Install

### ESP8266 Board Manager

1. Add URL:

   ```
   http://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```

2. Install "esp8266 by ESP8266 Community"

### Required Libraries (Library Manager)

- **PubSubClient** by Nick O'Leary (MQTT client)
- **DHT sensor library** by Adafruit
- **Adafruit Unified Sensor** by Adafruit

---

## Raspberry Pi Pico W Setup

### Install MicroPython

1. Hold BOOTSEL button, plug in USB
2. Download: <https://micropython.org/download/rp2-pico-w/>
3. Drag `.uf2` file to RPI-RP2 drive

### Install umqtt

```bash
pip install mpremote
mpremote mip install umqtt.simple
```

### Deploy Code

```bash
mpremote cp pico_motion_node/main.py :main.py
mpremote reset
```

### Monitor Serial

```bash
mpremote connect auto repl
```

---

## Alert Thresholds Summary

| Parameter | Safe | Warning | Alert/Critical |
|-----------|------|---------|----------------|
| Gas (ADC) | < 200 | 200–400 | > 400 |
| Temperature | < 35°C | 35–45°C | ≥ 45°C / ≥ 60°C FIRE |
| Motion | No detection | — | Any trigger |

---

## Testing with MQTT CLI

```bash
# Install mosquitto-clients
sudo apt install mosquitto-clients

# Publish test values
mosquitto_pub -h broker.hivemq.com -t "fire/gas" -m "850"
mosquitto_pub -h broker.hivemq.com -t "fire/temp" -m "65"
mosquitto_pub -h broker.hivemq.com -t "fire/motion" -m "1"

# Subscribe to all fire topics
mosquitto_sub -h broker.hivemq.com -t "fire/#" -v
```

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| MQTT not connecting | Check port 1883 is not blocked by firewall; try port 8883 (TLS) |
| ESP32 ADC noise | Add 100nF capacitor between A0 and GND |
| DHT11 read fails | Increase warm-up delay; check pull-up resistor |
| Pico PIR false triggers | Adjust onboard sensitivity pot; increase debounce to 1000ms |
| Node-RED "ui_led" missing | Run `npm install node-red-dashboard` in ~/.node-red |
| Dashboard shows no data | Check HiveMQ status at status.hivemq.com |

---

## Security Notes

> ⚠️ This system uses **broker.hivemq.com** (public broker).  
> For production, use a **private MQTT broker** with TLS and authentication.
> Consider: EMQX Cloud, AWS IoT Core, or self-hosted Mosquitto with TLS.

---

*FireWatch IoT — CNIC Lab End Semester Project*
