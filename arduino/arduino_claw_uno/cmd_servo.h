/*
 * cmd_servo.h — Servo Command Handlers
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_serial_protocol.h"

/**
 * Handle a servo command (sa, sv, sr, sd).
 */
void handle_servo_cmd(const claw_cmd_t *cmd);
