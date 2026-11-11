/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ir_fields.h"
#include "ir_frame.h"
#include "ir_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IR_PAYLOAD_MAX 48
#define IR_RAW_NONE UINT32_MAX
#define IR_PROTOCOL_MIN_CONSTRAINED_BITS 8

typedef struct {
    uint8_t byte;
    uint8_t bits;                       /* 1..8, the byte carries only its low `bits` bits on air */
} ir_bit_width_t;

typedef struct {
    uint8_t byte;
    uint8_t shift;                      /* LSB position inside the byte */
    uint8_t width;
} ir_bit_segment_t;

typedef struct {
    uint8_t id;                         /* ir_field_id_t */
    uint8_t segment_count;
    uint8_t value_count;                /* 0 means linear: raw = (value - base) / step */
    int32_t base;                       /* value of raw[0], or linear offset */
    int32_t step;                       /* 0 means 1 */
    const ir_bit_segment_t *segments;   /* least significant segment first */
    const uint32_t *raw;                /* raw[(value - base) / step], IR_RAW_NONE when unsupported */
} ir_bitfield_t;

typedef enum {
    IR_CHECK_COPY,                      /* target units = source units, e.g. NEC inverted bytes */
    IR_CHECK_SUM,
    IR_CHECK_XOR,
} ir_check_op_t;

enum {
    IR_CHECK_NIBBLE = 1U << 0,          /* units are nibbles: index byte * 2 + 1 for the low nibble */
    IR_CHECK_INVERT = 1U << 1,          /* store the bitwise complement */
    IR_CHECK_BYTE_TARGET = 1U << 2,     /* NIBBLE sources, but the target is a whole byte */
};

typedef struct {
    uint8_t op;
    uint8_t flags;
    uint8_t first;                      /* first source unit when units is NULL */
    uint8_t count;
    uint8_t target;                     /* first target unit */
    uint8_t bias;
    const uint8_t *units;               /* explicit source units, NULL for a contiguous range */
} ir_check_t;

typedef struct {
    const char *name;                   /* "frame", "off", "repeat" */
    const ir_layout_t *layout;
    uint8_t copies;                     /* protocol-mandated copies, >= 1 */
    bool msb_first;
    uint8_t length;                     /* payload bytes in transmit order */
    const ir_bit_width_t *widths;
    uint8_t width_count;
    const uint8_t *template_bytes;      /* NULL means zeros */
    const uint8_t *ignore;              /* bits not checked on decode; NULL means none */
    const ir_bitfield_t *fields;
    uint8_t field_count;
    const ir_check_t *checks;
    uint8_t check_count;
    const ir_values_t *implied;         /* values carried by this message, e.g. {power = off} */
} ir_message_t;

enum {
    IR_PROTOCOL_DECODE = 1U << 0,       /* known and verified, decodable */
    IR_PROTOCOL_UNCHECKED = 1U << 1,    /* decodable without checks, e.g. Sony, RC5 */
};

typedef struct {
    int32_t min;                        /* INT, DECI */
    int32_t max;
    int32_t step;
    uint32_t mask;                      /* BOOL, ENUM: bit per allowed value */
} ir_limit_t;

/* An all-zero mode limit marks a field the mode does not send: its value is ignored and not offered. */
static inline bool ir_limit_unused(const ir_limit_t *limit)
{
    return !limit->mask && !limit->step;
}

/* Capability description of an AC table or codeset, and the value limits verified for it. */
typedef struct {
    const char *category;
    const char *brand;
    const char *model;
    const char *source;
    const char *maturity;
    ir_values_t defaults;
    uint32_t limit_fields;
    ir_limit_t limits[IR_FIELD_COUNT];
    uint32_t mode_limit_fields[IR_MODE_COUNT];
    const ir_limit_t *mode_limits[IR_MODE_COUNT];   /* IR_FIELD_COUNT entries, override limits in that mode */
} ir_protocol_meta_t;

typedef struct {
    const char *id;
    uint32_t carrier_hz;
    uint32_t flags;
    const ir_message_t *messages;
    uint8_t message_count;
    const ir_message_t *hold;           /* sent while a key is held, NULL resends the message */
    const ir_protocol_meta_t *meta;     /* NULL: no description, only the field encodings limit values */
} ir_protocol_t;

typedef struct {
    const ir_message_t *message;
    ir_values_t values;
    uint8_t copies;                     /* identical copies matched */
    uint8_t holds;                      /* hold messages after the copies */
    size_t next;                        /* first pulse after the match */
} ir_decode_result_t;

/* Bit mask of the fields a message encodes. */
uint32_t ir_message_fields(const ir_message_t *message);
/* Fields any message encodes or implies. */
uint32_t ir_protocol_fields(const ir_protocol_t *protocol);
/* Effective limit of a field, mode-specific when mode >= 0. Returns false when the field is unlimited. */
bool ir_protocol_get_limit(const ir_protocol_t *protocol, ir_field_id_t field, int32_t mode, ir_limit_t *limit);
/* Checks every present value against the limits of values' mode, skipping unused fields; ESP_ERR_NOT_SUPPORTED when one is outside. */
esp_err_t ir_protocol_check(const ir_protocol_t *protocol, const ir_values_t *values);
/* Picks the message with the most implied values contained in values. */
const ir_message_t *ir_protocol_select(const ir_protocol_t *protocol, const ir_values_t *values);
size_t ir_protocol_max_pulses(const ir_protocol_t *protocol, uint32_t holds);
/* Hold messages that fill hold_ms, capped so the frame stays within IR_FRAME_MAX_PULSES. */
uint32_t ir_protocol_hold_count(const ir_protocol_t *protocol, const ir_values_t *values, uint32_t hold_ms);
/* Checks limits, then replaces frame contents. Missing fields give ESP_ERR_INVALID_ARG, unsupported values ESP_ERR_NOT_SUPPORTED. */
esp_err_t ir_protocol_encode(const ir_protocol_t *protocol, const ir_values_t *values, uint32_t holds, ir_frame_t *frame);
/* Errors: ESP_ERR_NOT_FOUND no match, ESP_ERR_INVALID_CRC checks failed, ESP_ERR_NOT_SUPPORTED unknown field value. */
esp_err_t ir_protocol_decode(const ir_protocol_t *protocol, const ir_frame_t *frame, size_t offset, ir_decode_result_t *result);
esp_err_t ir_protocol_validate(const ir_protocol_t *protocol);

#ifdef __cplusplus
}
#endif
