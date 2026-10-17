/*
 * claw_serial_protocol.cpp — Serial command parser and response formatter
 *
 * Lightweight JSON parser designed for ATmega328P constraints:
 *   - No dynamic memory allocation (all stack/static buffers)
 *   - Minimal string operations
 *   - Max 128-byte messages
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_serial_protocol.h"
#include "board_defs.h"

/* Forward declarations for command handlers (defined in cmd_*.cpp) */
extern void handle_gpio_cmd(const claw_cmd_t *cmd);
extern void handle_analog_cmd(const claw_cmd_t *cmd);
extern void handle_servo_cmd(const claw_cmd_t *cmd);
extern void handle_i2c_cmd(const claw_cmd_t *cmd);

/* ───────────────────── Static State ───────────────────── */
static char s_rx_buf[PROTO_MAX_CMD_LEN];
static uint8_t s_rx_pos = 0;

/* ───────────────────── Tiny JSON Helpers ───────────────────── */

/**
 * Find the value of a string key in a flat JSON object.
 * Returns pointer to the first char of the value, or NULL.
 * Only handles flat objects — no nesting.
 */
static const char *json_find_key(const char *json, const char *key)
{
    /* Build search pattern: "key": */
    char pattern[16];
    uint8_t plen = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (plen >= sizeof(pattern)) return NULL;

    const char *p = strstr(json, pattern);
    if (!p) return NULL;

    p += plen;
    /* Skip whitespace */
    while (*p == ' ') p++;
    return p;
}

/**
 * Extract a string value (without quotes) into buf.
 * Returns length written, or 0 on failure.
 */
static uint8_t json_get_string(const char *json, const char *key, char *buf, uint8_t buf_size)
{
    const char *val = json_find_key(json, key);
    if (!val || *val != '"') return 0;

    val++; /* skip opening quote */
    uint8_t i = 0;
    while (val[i] && val[i] != '"' && i < (buf_size - 1)) {
        buf[i] = val[i];
        i++;
    }
    buf[i] = '\0';
    return i;
}

/**
 * Extract an integer value. Returns the int, sets *found to true if present.
 */
static int16_t json_get_int(const char *json, const char *key, bool *found)
{
    const char *val = json_find_key(json, key);
    if (!val) {
        *found = false;
        return -1;
    }
    *found = true;
    return (int16_t)atoi(val);
}

/**
 * Decode a hex string into a byte array.
 * Returns number of bytes decoded.
 */
static uint8_t hex_decode(const char *hex, uint8_t *out, uint8_t max_len)
{
    uint8_t len = 0;
    while (hex[0] && hex[1] && len < max_len) {
        char hi = hex[0], lo = hex[1];
        uint8_t byte = 0;

        if (hi >= '0' && hi <= '9')      byte = (hi - '0') << 4;
        else if (hi >= 'A' && hi <= 'F') byte = (hi - 'A' + 10) << 4;
        else if (hi >= 'a' && hi <= 'f') byte = (hi - 'a' + 10) << 4;
        else break;

        if (lo >= '0' && lo <= '9')      byte |= (lo - '0');
        else if (lo >= 'A' && lo <= 'F') byte |= (lo - 'A' + 10);
        else if (lo >= 'a' && lo <= 'f') byte |= (lo - 'a' + 10);
        else break;

        out[len++] = byte;
        hex += 2;
    }
    return len;
}

/* ───────────────────── Parse Command ───────────────────── */

static bool parse_command(const char *json, claw_cmd_t *cmd)
{
    memset(cmd, 0, sizeof(claw_cmd_t));
    cmd->pin = -1;
    cmd->value = -1;
    cmd->count = -1;

    /* Extract command code (required) */
    if (json_get_string(json, "c", cmd->cmd, sizeof(cmd->cmd)) == 0) {
        return false;
    }

    /* Extract optional fields */
    bool found;

    cmd->pin = json_get_int(json, "p", &found);
    if (!found) cmd->pin = -1;

    cmd->value = json_get_int(json, "v", &found);
    if (!found) cmd->value = -1;

    int16_t addr = json_get_int(json, "a", &found);
    cmd->addr = found ? (uint8_t)addr : 0;

    cmd->count = json_get_int(json, "n", &found);
    if (!found) cmd->count = -1;

    /* Extract mode string */
    json_get_string(json, "m", cmd->mode, sizeof(cmd->mode));

    /* Extract hex data for I2C */
    char hex_buf[33]; /* 16 bytes * 2 hex chars + null */
    if (json_get_string(json, "d", hex_buf, sizeof(hex_buf)) > 0) {
        cmd->data_len = hex_decode(hex_buf, cmd->data, sizeof(cmd->data));
    }

    return true;
}

/* ───────────────────── Dispatch ───────────────────── */

static void dispatch_command(const claw_cmd_t *cmd)
{
    const char *c = cmd->cmd;

    /* Identity / Heartbeat */
    if (strcmp(c, CMD_IDENTIFY) == 0) {
        proto_respond_identify();
        return;
    }
    if (strcmp(c, CMD_PING) == 0) {
        proto_respond_ok();
        return;
    }

    /* GPIO commands */
    if (strcmp(c, CMD_PIN_MODE) == 0 ||
        strcmp(c, CMD_DIGITAL_WRITE) == 0 ||
        strcmp(c, CMD_DIGITAL_READ) == 0) {
        handle_gpio_cmd(cmd);
        return;
    }

    /* Analog commands */
    if (strcmp(c, CMD_ANALOG_READ) == 0 ||
        strcmp(c, CMD_ANALOG_WRITE) == 0) {
        handle_analog_cmd(cmd);
        return;
    }

    /* Servo commands */
    if (strcmp(c, CMD_SERVO_ATTACH) == 0 ||
        strcmp(c, CMD_SERVO_WRITE) == 0 ||
        strcmp(c, CMD_SERVO_READ) == 0 ||
        strcmp(c, CMD_SERVO_DETACH) == 0) {
        handle_servo_cmd(cmd);
        return;
    }

    /* I2C commands */
    if (strcmp(c, CMD_I2C_WRITE) == 0 ||
        strcmp(c, CMD_I2C_READ) == 0 ||
        strcmp(c, CMD_I2C_SCAN) == 0) {
        handle_i2c_cmd(cmd);
        return;
    }

    proto_respond_error("unknown cmd");
}

/* ───────────────────── Public API ───────────────────── */

void proto_init(void)
{
    Serial.begin(BOARD_SERIAL_BAUD);
    s_rx_pos = 0;

    /* Wait for serial port to be ready */
    while (!Serial) {
        ; /* Wait (relevant for boards with native USB) */
    }
}

bool proto_poll(void)
{
    while (Serial.available()) {
        char ch = Serial.read();

        if (ch == PROTO_DELIMITER || ch == '\r') {
            if (s_rx_pos == 0) continue; /* Skip empty lines */

            s_rx_buf[s_rx_pos] = '\0';

            claw_cmd_t cmd;
            if (parse_command(s_rx_buf, &cmd)) {
                dispatch_command(&cmd);
            } else {
                proto_respond_error("parse fail");
            }

            s_rx_pos = 0;
            return true;
        }

        if (s_rx_pos < (PROTO_MAX_CMD_LEN - 1)) {
            s_rx_buf[s_rx_pos++] = ch;
        } else {
            /* Buffer overflow — discard and reset */
            s_rx_pos = 0;
            proto_respond_error("overflow");
            return true;
        }
    }
    return false;
}

void proto_respond_ok(void)
{
    Serial.println(F("{\"ok\":true}"));
}

void proto_respond_ok_val(int value)
{
    Serial.print(F("{\"ok\":true,\"v\":"));
    Serial.print(value);
    Serial.println(F("}"));
}

void proto_respond_ok_data(const uint8_t *data, uint8_t len)
{
    Serial.print(F("{\"ok\":true,\"d\":\""));
    for (uint8_t i = 0; i < len; i++) {
        if (data[i] < 0x10) Serial.print('0');
        Serial.print(data[i], HEX);
    }
    Serial.println(F("\"}"));
}

void proto_respond_error(const char *message)
{
    Serial.print(F("{\"ok\":false,\"e\":\""));
    Serial.print(message);
    Serial.println(F("\"}"));
}

void proto_respond_identify(void)
{
    Serial.print(F("{\"board\":\""));
    Serial.print(F(BOARD_NAME));
    Serial.print(F("\",\"chip\":\""));
    Serial.print(F(BOARD_CHIP));
    Serial.print(F("\",\"sram\":"));
    Serial.print(BOARD_SRAM_BYTES);
    Serial.print(F(",\"flash\":"));
    Serial.print(BOARD_FLASH_BYTES);
    Serial.print(F(",\"pins\":{\"digital\":"));
    Serial.print(BOARD_DIGITAL_PINS);
    Serial.print(F(",\"analog\":"));
    Serial.print(BOARD_ANALOG_PINS);
    Serial.print(F("},\"caps\":[\"gpio\",\"adc\",\"pwm\",\"servo\",\"i2c\"]"));
    Serial.println(F("}"));
}

void proto_respond_i2c_scan(const uint8_t *addrs, uint8_t count)
{
    Serial.print(F("{\"ok\":true,\"devs\":["));
    for (uint8_t i = 0; i < count; i++) {
        if (i > 0) Serial.print(',');
        Serial.print(addrs[i]);
    }
    Serial.println(F("]}"));
}
