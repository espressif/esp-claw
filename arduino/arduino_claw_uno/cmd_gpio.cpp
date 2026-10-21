/*
 * cmd_gpio.cpp — GPIO Command Handlers
 *
 * Equivalent to ESP-Claw's lua_module_gpio:
 *   gpio.set_direction(pin, mode)  → {"c":"pm","p":PIN,"m":"out"}
 *   gpio.set_level(pin, level)     → {"c":"dw","p":PIN,"v":1}
 *   gpio.get_level(pin)            → {"c":"dr","p":PIN}
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_gpio.h"
#include "board_defs.h"

void handle_gpio_cmd(const claw_cmd_t *cmd)
{
    const char *c = cmd->cmd;

    /* ── pinMode ── */
    if (strcmp(c, CMD_PIN_MODE) == 0) {
        if (cmd->pin < 0 || cmd->pin >= BOARD_TOTAL_PINS) {
            proto_respond_error("bad pin");
            return;
        }
        if (!board_pin_is_user_safe((uint8_t)cmd->pin)) {
            proto_respond_error("pin reserved");
            return;
        }

        if (strcmp(cmd->mode, "out") == 0 || strcmp(cmd->mode, "output") == 0) {
            pinMode(cmd->pin, OUTPUT);
        } else if (strcmp(cmd->mode, "in") == 0 || strcmp(cmd->mode, "input") == 0) {
            pinMode(cmd->pin, INPUT);
        } else if (strcmp(cmd->mode, "pu") == 0 || strcmp(cmd->mode, "input_pullup") == 0) {
            pinMode(cmd->pin, INPUT_PULLUP);
        } else {
            proto_respond_error("bad mode");
            return;
        }

        proto_respond_ok();
        return;
    }

    /* ── digitalWrite ── */
    if (strcmp(c, CMD_DIGITAL_WRITE) == 0) {
        if (cmd->pin < 0 || cmd->pin >= BOARD_TOTAL_PINS) {
            proto_respond_error("bad pin");
            return;
        }
        if (cmd->value < 0) {
            proto_respond_error("no value");
            return;
        }

        digitalWrite(cmd->pin, cmd->value ? HIGH : LOW);
        proto_respond_ok();
        return;
    }

    /* ── digitalRead ── */
    if (strcmp(c, CMD_DIGITAL_READ) == 0) {
        if (cmd->pin < 0 || cmd->pin >= BOARD_TOTAL_PINS) {
            proto_respond_error("bad pin");
            return;
        }

        int val = digitalRead(cmd->pin);
        proto_respond_ok_val(val);
        return;
    }

    proto_respond_error("bad gpio cmd");
}
