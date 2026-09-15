/*
 * cmd_i2c.h — I2C Command Handlers
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_serial_protocol.h"

/**
 * Handle an I2C command (i2w, i2r, i2s).
 */
void handle_i2c_cmd(const claw_cmd_t *cmd);
