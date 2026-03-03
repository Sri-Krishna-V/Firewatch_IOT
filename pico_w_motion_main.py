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
import sys
from machine import Pin
from umqtt.simple import MQTTClient

# ── Config ──────────────────────────────────────
WIFI_SSID = "Nothing 2a plus"
WIFI_PASS = "12345678"
MQTT_BROKER = b"broker.hivemq.com"
MQTT_PORT = 1883
# Unique prefix avoids topic collision on public broker
MQTT_TOPIC  = b"fw2352/motion"
MQTT_CLIENT = b"fw2352_picow_motion"
STATUS_TOPIC = b"fw2352/status"
CMD_TOPIC   = b"fw2352/cmd/motion"   # arm / disarm commands from dashboard

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

# Arm/disarm state — controlled by dashboard command
armed = True

client = None

# ── Logger ───────────────────────────────────────
# Prefix every line with elapsed seconds so you can reconstruct a timeline
# from a paste of the REPL output.
def log(level, msg):
    t = time.ticks_ms() // 1000
    print(f"[{t:6d}s][{level}] {msg}")


# ── Status dump (paste this output when reporting an issue) ──────────────
def dump_status():
    wlan = network.WLAN(network.STA_IF)
    log("STAT", "─" * 40)
    log("STAT", f"WiFi connected : {wlan.isconnected()}")
    if wlan.isconnected():
        ip, mask, gw, dns = wlan.ifconfig()
        log("STAT", f"IP / GW        : {ip} / {gw}")
        log("STAT", f"RSSI           : {wlan.status('rssi')} dBm")
    log("STAT", f"MQTT broker    : {MQTT_BROKER.decode()}:{MQTT_PORT}")
    log("STAT", f"MQTT client ID : {MQTT_CLIENT.decode()}")
    log("STAT", f"Pub topic      : {MQTT_TOPIC.decode()}")
    log("STAT", f"Cmd topic      : {CMD_TOPIC.decode()}")
    log("STAT", f"Armed          : {armed}")
    log("STAT", f"Motion count   : {motion_count}")
    log("STAT", f"pending_motion : {pending_motion}")
    log("STAT", "─" * 40)


# ── WiFi ─────────────────────────────────────────


def connect_wifi():
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    log("WiFi", f"Connecting to SSID: '{WIFI_SSID}'...")
    wlan.connect(WIFI_SSID, WIFI_PASS)

    for i in range(20):
        status = wlan.status()  # numeric status code — helps diagnose auth failures
        if wlan.isconnected():
            ip, mask, gw, dns = wlan.ifconfig()
            rssi = wlan.status('rssi')
            log("WiFi", f"Connected!  IP={ip}  GW={gw}  RSSI={rssi} dBm")
            # Blink 3 times to signal WiFi OK
            for _ in range(3):
                led.on(); time.sleep(0.1)
                led.off(); time.sleep(0.1)
            return True
        log("WiFi", f"Waiting... attempt {i+1}/20  wlan.status()={status}")
        time.sleep(0.5)

    log("WiFi", f"FAILED after 10s  wlan.status()={wlan.status()}")
    log("WiFi", "  status codes: 0=idle 1=connecting 2=wrong_pass 3=no_ap 4=connected -1=fail")
    return False

# ── MQTT command callback ─────────────────────────────────


def cmd_callback(topic, msg):
    global armed
    try:
        log("CMD", f"topic={topic}  msg={msg}")
        if b"disarm" in msg:
            armed = False
            log("CMD", "Motion detection DISARMED by dashboard")
            client.publish(STATUS_TOPIC,
                           ujson.dumps({"node": "picow_motion", "armed": False}).encode())
            led.off()
        elif b"arm" in msg:
            armed = True
            log("CMD", "Motion detection ARMED by dashboard")
            client.publish(STATUS_TOPIC,
                           ujson.dumps({"node": "picow_motion", "armed": True}).encode())
            led.on()
        else:
            log("CMD", f"Unknown command ignored: {msg}")
    except Exception as e:
        log("ERR", f"cmd_callback raised: {e}")
        sys.print_exception(e)


# ── MQTT ─────────────────────────────────────────


def connect_mqtt():
    global client
    log("MQTT", f"Connecting to {MQTT_BROKER.decode()}:{MQTT_PORT}  client_id={MQTT_CLIENT.decode()}")
    log("MQTT", "(this can take 5-15s on first attempt — DNS + TCP handshake)")
    try:
        client = MQTTClient(
            MQTT_CLIENT,
            MQTT_BROKER,
            port=MQTT_PORT,
            keepalive=120,
            ssl=False
        )
        log("MQTT", "MQTTClient object created — calling connect()...")
        client.connect()
        log("MQTT", "TCP connection OK — broker accepted CONNECT")

        # Register command callback and subscribe
        client.set_callback(cmd_callback)
        client.subscribe(CMD_TOPIC)
        log("MQTT", f"Subscribed to {CMD_TOPIC.decode()}")

        # Publish online status
        status = ujson.dumps({
            "node": "picow_motion",
            "status": "online",
            "pin": PIR_PIN
        })
        client.publish(STATUS_TOPIC, status.encode())
        log("MQTT", f"Published online status to {STATUS_TOPIC.decode()}")
        led.on()
        return True
    except Exception as e:
        log("ERR", f"MQTT connect failed: {e}")
        sys.print_exception(e)   # <-- full traceback helps diagnose socket/DNS errors
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
        log("PUB", f"{MQTT_TOPIC.decode()} ← {payload}")
    except Exception as e:
        log("ERR", f"publish_motion failed: {e}")
        sys.print_exception(e)

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
        log("HB", "PINGREQ sent")
        # Also publish last known motion state so subscribers stay in sync
        publish_motion(pending_motion is True, motion_count)
        dump_status()  # prints full state — paste this when reporting an issue
    except Exception as e:
        log("ERR", f"Heartbeat/ping failed: {e}")
        sys.print_exception(e)
        log("HB", "Attempting reconnect...")
        connect_wifi()
        connect_mqtt()

# ── Main ──────────────────────────────────────────


def main():
    log("BOOT", "═" * 44)
    log("BOOT", " FireWatch IoT — Pico W Motion Node")
    log("BOOT", f" MicroPython {sys.version}")
    log("BOOT", f" PIR GPIO{PIR_PIN}   debounce={DEBOUNCE_MS}ms")
    log("BOOT", f" Broker  {MQTT_BROKER.decode()}:{MQTT_PORT}")
    log("BOOT", f" Topics  pub={MQTT_TOPIC.decode()}  cmd={CMD_TOPIC.decode()}")
    log("BOOT", "═" * 44)

    led.off()
    time.sleep(2)  # Brief settle

    if not connect_wifi():
        log("FATAL", "No WiFi — check SSID/password then hard-reset the Pico")
        # Rapid blink so you can distinguish WiFi failure from MQTT failure
        while True:
            for _ in range(3):
                led.on(); time.sleep(0.1)
                led.off(); time.sleep(0.1)
            time.sleep(1)

    time.sleep(1)

    if not connect_mqtt():
        log("FATAL", "No MQTT — check broker hostname, port, and network firewall")
        # Slow blink = MQTT failure (distinct from WiFi fast-blink)
        while True:
            led.on(); time.sleep(0.5)
            led.off(); time.sleep(0.5)

    # Attach interrupt
    pir.irq(trigger=Pin.IRQ_RISING | Pin.IRQ_FALLING, handler=pir_handler)
    log("BOOT", f"PIR IRQ attached on GPIO{PIR_PIN}")
    log("BOOT", "Motion node READY — entering main loop\n")

    # Main loop: consume IRQ flags + keep-alive
    hb_counter = 0
    global pending_motion, motion_count
    while True:
        try:
            time.sleep(PUBLISH_INTERVAL)
            hb_counter += 1

            # Service the TCP socket — critical to avoid silent broker disconnects
            client.check_msg()

            # Drain any pending IRQ event (set by pir_handler)
            if pending_motion is not None:
                if armed:
                    detected = pending_motion
                    pending_motion = None
                    if detected:
                        motion_count += 1
                        log("PIR", f"*** MOTION DETECTED  event=#{motion_count} ***")
                        led.on()
                    else:
                        log("PIR", "Motion ended — area clear")
                        led.off()
                    publish_motion(detected, motion_count)
                else:
                    pending_motion = None  # Clear event silently when disarmed
                    log("PIR", "Motion event ignored — system DISARMED")

            # Keep-alive + state sync every 15 iterations (~30s, well within 120s keepalive)
            if hb_counter % 15 == 0:
                heartbeat()

        except KeyboardInterrupt:
            log("BOOT", "KeyboardInterrupt — shutting down cleanly")
            publish_motion(False, motion_count)
            client.disconnect()
            led.off()
            break
        except Exception as e:
            log("ERR", f"Main loop exception: {e}")
            sys.print_exception(e)  # full traceback — paste this when asking for help
            log("ERR", "Waiting 3s then attempting full reconnect...")
            time.sleep(3)
            connect_wifi()
            connect_mqtt()


if __name__ == "__main__":
    # Set PRODUCTION = True once sensor is confirmed working.
    # HC-SR501 needs ~60s on first power-on to self-calibrate.
    PRODUCTION = False
    warmup = 60 if PRODUCTION else 5
    log("BOOT", f"Sensor warm-up ({warmup}s){'  [PRODUCTION]' if PRODUCTION else '  [TEST \u2014 set PRODUCTION=True when deploying]'}...")
    time.sleep(warmup)
    main()
