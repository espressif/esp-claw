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
#include "ir_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IR_BITS_MAX 384
#define IR_LAYOUT_TOLERANCE_PCT 25
#define IR_LAYOUT_TOLERANCE_MIN_US 100
#define IR_LAYOUT_SYNC_TRIES 16
#define IR_LAYOUT_SYMBOL_BITS_MAX 2
#define IR_LAYOUT_SYMBOLS_MAX (1U << IR_LAYOUT_SYMBOL_BITS_MAX)

typedef struct {
    uint16_t count;
    uint8_t data[IR_BITS_MAX / 8];      /* bit i is the i-th transmitted bit, LSB of data[0] first */
} ir_bits_t;

typedef struct {
    uint16_t mark_us;
    uint16_t space_us;
} ir_symbol_t;

typedef enum {
    IR_CODING_PAIR,                     /* each symbol is one mark/space pair carrying symbol_bits bits */
    IR_CODING_BIPHASE,                  /* each bit is two half periods with opposite levels */
} ir_coding_t;

typedef enum {
    IR_STEP_MARK,
    IR_STEP_SPACE,
    IR_STEP_BITS,
    IR_STEP_PERIOD,                     /* space until the copy lasts value_us from its start */
} ir_step_kind_t;

typedef struct {
    uint8_t kind;
    uint16_t count;                     /* BITS */
    uint32_t value_us;                  /* MARK, SPACE, PERIOD */
} ir_step_t;

#define IR_MARK(us) {.kind = IR_STEP_MARK, .value_us = (us)}
#define IR_SPACE(us) {.kind = IR_STEP_SPACE, .value_us = (us)}
#define IR_BITS(n) {.kind = IR_STEP_BITS, .count = (n)}
#define IR_PERIOD(us) {.kind = IR_STEP_PERIOD, .value_us = (us)}

typedef struct {
    ir_coding_t coding;
    uint8_t one_level;                  /* BIPHASE: level of the first half of a one, RC5 = 0 */
    uint8_t symbol_bits;                /* PAIR: bits per symbol, 0 or 1 for two symbols, 2 for four */
    uint16_t half_us;                   /* BIPHASE */
    ir_symbol_t symbols[IR_LAYOUT_SYMBOLS_MAX]; /* PAIR: indexed by value, the first sent bit is the high bit */
    const ir_step_t *steps;
    uint16_t step_count;
} ir_layout_t;

uint16_t ir_layout_bit_count(const ir_layout_t *layout);
size_t ir_layout_max_pulses(const ir_layout_t *layout);
uint32_t ir_layout_max_duration_us(const ir_layout_t *layout);
esp_err_t ir_layout_validate(const ir_layout_t *layout, bool decodable);
esp_err_t ir_layout_encode(const ir_layout_t *layout, const ir_bits_t *bits, ir_frame_t *frame);
/* Matches one full copy at offset, searching later marks for the header. *next is the first pulse after it. */
esp_err_t ir_layout_decode(const ir_layout_t *layout, const ir_frame_t *frame, size_t offset, ir_bits_t *bits, size_t *next);

static inline bool ir_bits_get(const ir_bits_t *bits, uint16_t index)
{
    return (bits->data[index >> 3] >> (index & 7U)) & 1U;
}

#ifdef __cplusplus
}
#endif
