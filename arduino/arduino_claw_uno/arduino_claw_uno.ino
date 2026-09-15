/*
 * arduino_claw_uno.ino — ESP-Claw Arduino UNO Peripheral Controller
 *
 * This sketch turns an Arduino UNO into a remote peripheral controller
 * for the ESP-Claw AI Agent framework. The UNO exposes its hardware
 * capabilities (GPIO, ADC, PWM, Servo, I2C) through a compact JSON
 * serial protocol.
 *
 * Architecture: Split-Brain
 *   ┌──────────────┐   Serial/UART   ┌──────────────┐
 *   │  HOST BRAIN  │◄───────────────►│  ARDUINO UNO │
 *   │  (ESP32 or   │  JSON commands  │  (this code) │
 *   │   PC bridge) │                 │              │
 *   └──────────────┘                 └──────────────┘
 *
 * The host runs the full ESP-Claw stack (LLM, event router, memory,
 * skills, capabilities) and sends hardware commands to the UNO over
 * serial. The UNO executes them and returns results.
 *
 * Supported commands:
 *   id  — Board identification
 *   pi  — Ping / heartbeat
 *   pm  — pinMode
 *   dw  — digitalWrite
 *   dr  — digitalRead
 *   ar  — analogRead (ADC)
 *   pw  — analogWrite (PWM)
 *   sa  — Servo attach
 *   sv  — Servo write angle
 *   sr  — Servo read angle
 *   sd  — Servo detach
 *   i2w — I2C write
 *   i2r — I2C read
 *   i2s — I2C bus scan
 *
 * Protocol: newline-delimited JSON, 115200 baud
 * Example:
 *   Send:    {"c":"dw","p":13,"v":1}
 *   Receive: {"ok":true}
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "claw_serial_protocol.h"
#include "board_defs.h"

/* ───────────────────── Status LED ───────────────────── */
#define STATUS_LED_PIN      BOARD_LED_BUILTIN
#define STATUS_BLINK_MS     1000    /* Blink interval when idle */
#define ACTIVITY_FLASH_MS   50      /* Brief flash on command */

static unsigned long s_last_blink = 0;
static bool s_led_state = false;
static unsigned long s_activity_until = 0;

/**
 * Blink the built-in LED to indicate the firmware is running.
 * Briefly holds LED on when a command is processed.
 */
static void update_status_led(bool had_activity)
{
    unsigned long now = millis();

    if (had_activity) {
        s_activity_until = now + ACTIVITY_FLASH_MS;
        digitalWrite(STATUS_LED_PIN, HIGH);
        return;
    }

    /* Activity flash still active */
    if (now < s_activity_until) {
        return;
    }

    /* Normal idle blink */
    if ((now - s_last_blink) >= STATUS_BLINK_MS) {
        s_last_blink = now;
        s_led_state = !s_led_state;
        digitalWrite(STATUS_LED_PIN, s_led_state ? HIGH : LOW);
    }
}

/* ───────────────────── Arduino Entry Points ───────────────────── */

void setup()
{
    /* Initialize status LED */
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, HIGH);

    /* Initialize serial protocol (opens Serial at 115200) */
    proto_init();

    /* Send startup identification */
    delay(100); /* Brief delay for serial to stabilize */
    proto_respond_identify();

    /* Status LED off after init */
    digitalWrite(STATUS_LED_PIN, LOW);
}

void loop()
{
    /* Poll for serial commands (non-blocking) */
    bool had_command = proto_poll();

    /* Update status LED */
    update_status_led(had_command);
}
