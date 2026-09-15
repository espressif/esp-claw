/*
 * cmd_gpio.h — GPIO Command Handlers
 *
 * Maps to ESP-Claw's lua_module_gpio functionality:
 *   - pinMode (input, output, input_pullup)
 *   - digitalWrite (HIGH/LOW)
 *   - digitalRead
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_serial_protocol.h"

/**
 * Handle a GPIO command (pm, dw, dr).
 * Called by the protocol dispatcher.
 */
void handle_gpio_cmd(const claw_cmd_t *cmd);
