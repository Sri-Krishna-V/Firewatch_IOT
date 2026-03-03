# ================================================
#  FireWatch IoT — Raspberry Pi Pico W Motion Node
#  Sensor: HC-SR501 PIR Motion Sensor (GPIO15)
#  Topic:  fire/motion
#  Broker: broker.hivemq.com:1883
#  Runtime: MicroPython (v1.20+)
# ================================================
#  Requirements:
#    - MicroPython firmware on Pico W
#    - umqtt.simple (bundled with MicroPython)
#    Install: mpremote mip install umqtt.simple
# ================================================

import network
import time
import ujson
from machine import Pin
from umqtt.simple import MQTTClient

# ── Config ──────────────────────────────────────
WIFI_SSID = "YOUR_WIFI_SSID"
WIFI_PASS = "YOUR_WIFI_PASSWORD"
MQTT_BROKER = b"broker.hivemq.com"
MQTT_PORT = 1883
# Unique prefix avoids topic collision on public broker
MQTT_TOPIC = b"fw2352/motion"
MQTT_CLIENT = b"fw2352_picow_motion"
STATUS_TOPIC = b"fw2352/status"

# ── Pins ─────────────────────────────────────────
PIR_PIN = 15    # GPIO15 — PIR signal output
LED_PIN = "LED"  # Pico W onboard LED

pir = Pin(PIR_PIN, Pin.IN, Pin.PULL_DOWN)
led = Pin(LED_PIN, Pin.OUT)

# ── State ────────────────────────────────────────
motion_count = 0
last_motion = 0
last_publish = 0
DEBOUNCE_MS = 500   # Ignore re-triggers within 500ms
PUBLISH_INTERVAL = 2   # seconds (heartbeat even if no motion)

# IRQ-safe flag: set by interrupt, consumed by main loop
pending_motion = None  # True = detected, False = ended, None = no event

client = None

# ── WiFi ─────────────────────────────────────────


def connect_wifi():
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    print(f"[WiFi] Connecting to {WIFI_SSID}...")
    wlan.connect(WIFI_SSID, WIFI_PASS)

    for i in range(20):
        if wlan.isconnected():
            ip = wlan.ifconfig()[0]
            print(f"[WiFi] Connected! IP: {ip}")
            # Blink 3 times to signal WiFi OK
            for _ in range(3):
                led.on()
                time.sleep(0.1)
                led.off()
                time.sleep(0.1)
            return True
        print(".", end="")
        time.sleep(0.5)

    print("\n[WiFi] Connection FAILED")
    return False

# ── MQTT ─────────────────────────────────────────


def connect_mqtt():
    global client
    print(f"[MQTT] Connecting to {MQTT_BROKER.decode()}...")
    try:
        client = MQTTClient(
            MQTT_CLIENT,
            MQTT_BROKER,
            port=MQTT_PORT,
            keepalive=60
        )
        client.connect()
        print("[MQTT] Connected to HiveMQ!")

        # Publish online status
        status = ujson.dumps({
            "node": "picow_motion",
            "status": "online",
            "pin": PIR_PIN
        })
        client.publish(STATUS_TOPIC, status.encode())
        led.on()
        return True
    except Exception as e:
        print(f"[MQTT] Connection failed: {e}")
        return False

# ── Publish payload ───────────────────────────────


def publish_motion(detected: bool, count: int):
    payload = ujson.dumps({
        "motion": detected,
        "count": count,
        "status": "detected" if detected else "clear"
    })
    try:
        client.publish(MQTT_TOPIC, payload.encode())
        print(f"[MOTION] Published → {MQTT_TOPIC.decode()} : {payload}")
    except Exception as e:
        print(f"[MOTION] Publish error: {e}")

# ── PIR Interrupt Handler ─────────────────────────
# IMPORTANT: only set a flag here — never do socket I/O inside an ISR.
# The main loop reads and acts on pending_motion safely.


def pir_handler(pin):
    global last_motion, pending_motion
    now = time.ticks_ms()

    # Debounce
    if time.ticks_diff(now, last_motion) < DEBOUNCE_MS:
        return

    last_motion = now
    pending_motion = (pin.value() == 1)  # True = rising, False = falling

# ── Heartbeat (keep-alive + publish current state) ─


def heartbeat():
    try:
        client.ping()
        # Also publish last known motion state so subscribers stay in sync
        publish_motion(pending_motion is True, motion_count)
    except Exception as e:
        print(f"[MQTT] Ping failed: {e}")
        connect_wifi()
        connect_mqtt()

# ── Main ──────────────────────────────────────────


def main():
    print("\n[FireWatch] Pico W Motion Detection Node Starting...")
    print(f"[FireWatch] PIR on GPIO{PIR_PIN}")

    led.off()
    time.sleep(2)  # Sensor warm-up

    if not connect_wifi():
        print("[FATAL] No WiFi — halting")
        # Blink SOS
        while True:
            for _ in range(3):
                led.on()
                time.sleep(0.1)
                led.off()
                time.sleep(0.1)
            time.sleep(1)

    time.sleep(1)

    if not connect_mqtt():
        print("[FATAL] No MQTT — halting")
        while True:
            led.on()
            time.sleep(0.5)
            led.off()
            time.sleep(0.5)

    # Attach interrupt
    pir.irq(trigger=Pin.IRQ_RISING | Pin.IRQ_FALLING, handler=pir_handler)
    print("[FireWatch] PIR interrupt attached. Monitoring...")
    print("[FireWatch] Motion node READY.\n")

    # Main loop: consume IRQ flags + keep-alive
    hb_counter = 0
    while True:
        try:
            time.sleep(PUBLISH_INTERVAL)
            hb_counter += 1

            # Drain any pending IRQ event (set by pir_handler)
            global pending_motion, motion_count
            if pending_motion is not None:
                detected = pending_motion
                pending_motion = None
                if detected:
                    motion_count += 1
                    print(
                        f"[PIR] *** MOTION DETECTED! Event #{motion_count} ***")
                    led.on()
                else:
                    print("[PIR] Motion ended — area clear")
                    led.off()
                publish_motion(detected, motion_count)

            # Keep-alive + state sync every 30 iterations (~60s)
            if hb_counter % 30 == 0:
                heartbeat()

        except KeyboardInterrupt:
            print("\n[FireWatch] Shutting down...")
            publish_motion(False, motion_count)
            client.disconnect()
            led.off()
            break
        except Exception as e:
            print(f"[ERROR] {e} — reconnecting...")
            time.sleep(3)
            connect_wifi()   # Reconnect WiFi first, then MQTT
            connect_mqtt()


if __name__ == "__main__":
    # Allow PIR to stabilize (60s warm-up for HC-SR501)
    print("[PIR] Sensor warm-up (5s)...")
    time.sleep(5)
    main()
