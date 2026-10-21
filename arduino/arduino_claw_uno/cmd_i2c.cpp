/*
 * cmd_i2c.cpp — I2C Command Handlers
 *
 * Uses the Arduino Wire library. Maps to ESP-Claw's lua_module_i2c.
 *
 * Commands:
 *   {"c":"i2w","a":60,"d":"48656C"}   → write bytes to addr 0x3C
 *   {"c":"i2r","a":60,"n":2}          → read 2 bytes from addr 0x3C
 *   {"c":"i2s"}                       → scan bus, return found addresses
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_i2c.h"
#include "board_defs.h"
#include <Wire.h>

static bool s_wire_init = false;

static void ensure_wire(void)
{
    if (!s_wire_init) {
        Wire.begin();
        s_wire_init = true;
    }
}

void handle_i2c_cmd(const claw_cmd_t *cmd)
{
    ensure_wire();
    const char *c = cmd->cmd;

    /* ── I2C Write ── */
    if (strcmp(c, CMD_I2C_WRITE) == 0) {
        if (cmd->addr == 0 || cmd->addr > 127) {
            proto_respond_error("bad addr");
            return;
        }
        if (cmd->data_len == 0) {
            proto_respond_error("no data");
            return;
        }

        Wire.beginTransmission(cmd->addr);
        Wire.write(cmd->data, cmd->data_len);
        uint8_t result = Wire.endTransmission();

        if (result == 0) {
            proto_respond_ok();
        } else {
            proto_respond_error("i2c nack");
        }
        return;
    }

    /* ── I2C Read ── */
    if (strcmp(c, CMD_I2C_READ) == 0) {
        if (cmd->addr == 0 || cmd->addr > 127) {
            proto_respond_error("bad addr");
            return;
        }
        if (cmd->count <= 0 || cmd->count > 16) {
            proto_respond_error("n 1-16");
            return;
        }

        uint8_t n = (uint8_t)cmd->count;
        uint8_t received = Wire.requestFrom(cmd->addr, n);

        uint8_t buf[16];
        uint8_t i = 0;
        while (Wire.available() && i < n) {
            buf[i++] = Wire.read();
        }

        if (i == 0) {
            proto_respond_error("i2c no data");
        } else {
            proto_respond_ok_data(buf, i);
        }
        return;
    }

    /* ── I2C Scan ── */
    if (strcmp(c, CMD_I2C_SCAN) == 0) {
        uint8_t found[16]; /* Max 16 devices in scan results */
        uint8_t count = 0;

        for (uint8_t addr = 1; addr < 128 && count < 16; addr++) {
            Wire.beginTransmission(addr);
            uint8_t result = Wire.endTransmission();
            if (result == 0) {
                found[count++] = addr;
            }
        }

        proto_respond_i2c_scan(found, count);
        return;
    }

    proto_respond_error("bad i2c cmd");
}
