/*
 * cmd_analog.h — Analog I/O Command Handlers (ADC + PWM)
 *
 * Maps to ESP-Claw's lua_module_adc (analogRead) and
 * PWM portion of lua_module_mcpwm (analogWrite).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_serial_protocol.h"

/**
 * Handle an analog command (ar, pw).
 * Called by the protocol dispatcher.
 */
void handle_analog_cmd(const claw_cmd_t *cmd);
