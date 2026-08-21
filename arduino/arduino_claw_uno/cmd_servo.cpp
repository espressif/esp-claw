/*
 * cmd_servo.cpp — Servo Command Handlers
 *
 * Uses the Arduino Servo library. Limited to BOARD_SERVO_MAX
 * simultaneous servos to conserve SRAM.
 *
 * Commands:
 *   {"c":"sa","p":9}         → attach servo on pin 9
 *   {"c":"sv","p":9,"v":90}  → write 90° to servo on pin 9
 *   {"c":"sr","p":9}         → read current angle from servo on pin 9
 *   {"c":"sd","p":9}         → detach servo on pin 9
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_servo.h"
#include "board_defs.h"
#include <Servo.h>

/* ── Static servo pool (max 4 to save RAM) ── */
typedef struct {
    Servo servo;
    int8_t pin;   /* -1 = unused slot */
} servo_slot_t;

static servo_slot_t s_servos[BOARD_SERVO_MAX];
static bool s_servo_init = false;

static void ensure_init(void)
{
    if (!s_servo_init) {
        for (uint8_t i = 0; i < BOARD_SERVO_MAX; i++) {
            s_servos[i].pin = -1;
        }
        s_servo_init = true;
    }
}

static int8_t find_slot_by_pin(int8_t pin)
{
    for (uint8_t i = 0; i < BOARD_SERVO_MAX; i++) {
        if (s_servos[i].pin == pin) return (int8_t)i;
    }
    return -1;
}

static int8_t find_free_slot(void)
{
    for (uint8_t i = 0; i < BOARD_SERVO_MAX; i++) {
        if (s_servos[i].pin == -1) return (int8_t)i;
    }
    return -1;
}

void handle_servo_cmd(const claw_cmd_t *cmd)
{
    ensure_init();
    const char *c = cmd->cmd;

    if (cmd->pin < 0 || cmd->pin >= BOARD_DIGITAL_PINS) {
        proto_respond_error("bad pin");
        return;
    }

    /* ── Servo Attach ── */
    if (strcmp(c, CMD_SERVO_ATTACH) == 0) {
        /* Check if already attached */
        if (find_slot_by_pin((int8_t)cmd->pin) >= 0) {
            proto_respond_ok(); /* Already attached, idempotent */
            return;
        }
        int8_t slot = find_free_slot();
        if (slot < 0) {
            proto_respond_error("no servo slot");
            return;
        }
        s_servos[slot].servo.attach(cmd->pin);
        s_servos[slot].pin = (int8_t)cmd->pin;
        proto_respond_ok();
        return;
    }

    /* ── Servo Write ── */
    if (strcmp(c, CMD_SERVO_WRITE) == 0) {
        int8_t slot = find_slot_by_pin((int8_t)cmd->pin);
        if (slot < 0) {
            proto_respond_error("not attached");
            return;
        }
        if (cmd->value < 0 || cmd->value > 180) {
            proto_respond_error("angle 0-180");
            return;
        }
        s_servos[slot].servo.write(cmd->value);
        proto_respond_ok();
        return;
    }

    /* ── Servo Read ── */
    if (strcmp(c, CMD_SERVO_READ) == 0) {
        int8_t slot = find_slot_by_pin((int8_t)cmd->pin);
        if (slot < 0) {
            proto_respond_error("not attached");
            return;
        }
        int angle = s_servos[slot].servo.read();
        proto_respond_ok_val(angle);
        return;
    }

    /* ── Servo Detach ── */
    if (strcmp(c, CMD_SERVO_DETACH) == 0) {
        int8_t slot = find_slot_by_pin((int8_t)cmd->pin);
        if (slot < 0) {
            proto_respond_ok(); /* Already detached, idempotent */
            return;
        }
        s_servos[slot].servo.detach();
        s_servos[slot].pin = -1;
        proto_respond_ok();
        return;
    }

    proto_respond_error("bad servo cmd");
}
