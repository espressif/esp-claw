/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IR_FRAME_MAX_PULSES 4096
#define IR_FRAME_MAX_PULSE_US 10000000
#define IR_FRAME_MIN_CARRIER_HZ 1000
#define IR_FRAME_MAX_CARRIER_HZ 1000000
#define IR_FRAME_GLITCH_US 100

/* Even indices are marks and odd indices are spaces. carrier_hz 0 means unknown, e.g. a received frame. */
typedef struct {
    size_t capacity;
    size_t count;
    uint32_t carrier_hz;
    uint32_t pulses_us[];
} ir_frame_t;

size_t ir_frame_size(size_t capacity);
esp_err_t ir_frame_init(ir_frame_t *frame, size_t capacity);
ir_frame_t *ir_frame_create(size_t capacity);
ir_frame_t *ir_frame_clone(const ir_frame_t *source);
void ir_frame_delete(ir_frame_t *frame);
esp_err_t ir_frame_validate(const ir_frame_t *frame);
/* Level 1 is a mark. Equal levels merge and a leading space is dropped. */
esp_err_t ir_frame_push_level(ir_frame_t *frame, int level, uint32_t duration_us);
esp_err_t ir_frame_push_pair(ir_frame_t *frame, uint32_t mark_us, uint32_t space_us);
/* Folds pulses shorter than glitch_us into their neighbours; glitches at either end are dropped. */
void ir_frame_normalize(ir_frame_t *frame, uint32_t glitch_us);
uint64_t ir_frame_duration_us(const ir_frame_t *frame);

#ifdef __cplusplus
}
#endif
