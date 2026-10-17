/*
 * cmd_analog.cpp — Analog I/O Command Handlers (ADC + PWM)
 *
 * Equivalent to ESP-Claw's lua_module_adc + lua_module_mcpwm:
 *   analogRead(pin)          → {"c":"ar","p":0}
 *   analogWrite(pin, value)  → {"c":"pw","p":9,"v":128}
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_analog.h"
#include "board_defs.h"

void handle_analog_cmd(const claw_cmd_t *cmd)
{
    const char *c = cmd->cmd;

    /* ── analogRead ── */
    if (strcmp(c, CMD_ANALOG_READ) == 0) {
        if (cmd->pin < 0) {
            proto_respond_error("no pin");
            return;
        }

        /* Accept both A0-style (14-19) and raw 0-5 */
        uint8_t apin = (uint8_t)cmd->pin;
        if (apin <= 5) {
            apin = A0 + apin;
        }

        if (!board_pin_has_adc(apin)) {
            proto_respond_error("not adc pin");
            return;
        }

        int val = analogRead(apin);
        proto_respond_ok_val(val);
        return;
    }

    /* ── analogWrite / PWM ── */
    if (strcmp(c, CMD_ANALOG_WRITE) == 0) {
        if (cmd->pin < 0) {
            proto_respond_error("no pin");
            return;
        }
        if (cmd->value < 0 || cmd->value > 255) {
            proto_respond_error("val 0-255");
            return;
        }
        if (!board_pin_has_pwm((uint8_t)cmd->pin)) {
            proto_respond_error("not pwm pin");
            return;
        }

        analogWrite(cmd->pin, cmd->value);
        proto_respond_ok();
        return;
    }

    proto_respond_error("bad analog cmd");
}
