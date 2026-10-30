/*
 * claw_serial_protocol.h — Compact JSON Serial Command Protocol
 *
 * Handles parsing incoming serial commands and formatting responses.
 * Protocol: newline-delimited JSON, max 128 bytes per message.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <Arduino.h>
#include "board_defs.h"

/* ───────────────────── Command Codes ───────────────────── */
/* Keep short to save bandwidth and parsing time */
#define CMD_IDENTIFY      "id"    /* Board identification */
#define CMD_PIN_MODE      "pm"    /* pinMode */
#define CMD_DIGITAL_WRITE "dw"    /* digitalWrite */
#define CMD_DIGITAL_READ  "dr"    /* digitalRead */
#define CMD_ANALOG_READ   "ar"    /* analogRead */
#define CMD_ANALOG_WRITE  "pw"    /* analogWrite / PWM */
#define CMD_SERVO_ATTACH  "sa"    /* Servo attach */
#define CMD_SERVO_WRITE   "sv"    /* Servo write angle */
#define CMD_SERVO_READ    "sr"    /* Servo read angle */
#define CMD_SERVO_DETACH  "sd"    /* Servo detach */
#define CMD_I2C_WRITE     "i2w"   /* I2C write */
#define CMD_I2C_READ      "i2r"   /* I2C read */
#define CMD_I2C_SCAN      "i2s"   /* I2C bus scan */
#define CMD_PING          "pi"    /* Ping / heartbeat */

/* ───────────────────── Protocol Constants ───────────────────── */
#define PROTO_MAX_CMD_LEN   BOARD_SERIAL_RX_BUF
#define PROTO_MAX_RESP_LEN  BOARD_SERIAL_TX_BUF
#define PROTO_DELIMITER     '\n'

/* ───────────────────── Parsed Command Structure ───────────────────── */
typedef struct {
    char cmd[4];       /* Command code (e.g., "dw", "ar") */
    int16_t pin;       /* Pin number (-1 if not provided) */
    int16_t value;     /* Value (-1 if not provided) */
    uint8_t addr;      /* I2C address (0 if not provided) */
    char mode[8];      /* Mode string for pinMode ("in","out","pu") */
    uint8_t data[16];  /* Raw data bytes for I2C write */
    uint8_t data_len;  /* Length of data[] */
    int16_t count;     /* Byte count for I2C read (-1 if not provided) */
} claw_cmd_t;

/* ───────────────────── Command Dispatcher Callback ───────────────────── */
typedef void (*cmd_handler_fn)(const claw_cmd_t *cmd);

/* ───────────────────── Protocol API ───────────────────── */

/**
 * Initialize the serial protocol (call in setup()).
 */
void proto_init(void);

/**
 * Poll for incoming serial data, parse complete commands, and dispatch.
 * Call this in loop() — it is non-blocking.
 * Returns true if a command was processed this call.
 */
bool proto_poll(void);

/**
 * Send a success response with no extra data.
 * Output: {"ok":true}\n
 */
void proto_respond_ok(void);

/**
 * Send a success response with an integer value.
 * Output: {"ok":true,"v":VALUE}\n
 */
void proto_respond_ok_val(int value);

/**
 * Send a success response with a hex-encoded data buffer.
 * Output: {"ok":true,"d":"HEXDATA"}\n
 */
void proto_respond_ok_data(const uint8_t *data, uint8_t len);

/**
 * Send an error response.
 * Output: {"ok":false,"e":"message"}\n
 */
void proto_respond_error(const char *message);

/**
 * Send the board identification response.
 * Output: {"board":"uno","chip":"ATmega328P","caps":[...]}\n
 */
void proto_respond_identify(void);

/**
 * Send an I2C scan result.
 * Output: {"ok":true,"devs":[addr1,addr2,...]}\n
 */
void proto_respond_i2c_scan(const uint8_t *addrs, uint8_t count);
