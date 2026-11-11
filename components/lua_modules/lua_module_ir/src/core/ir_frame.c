/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ir_frame.h"

#include <stdlib.h>
#include <string.h>

size_t ir_frame_size(size_t capacity)
{
    if (!capacity || capacity > IR_FRAME_MAX_PULSES) return 0;
    return sizeof(ir_frame_t) + capacity * sizeof(uint32_t);
}

esp_err_t ir_frame_init(ir_frame_t *frame, size_t capacity)
{
    if (!frame || !ir_frame_size(capacity)) return ESP_ERR_INVALID_ARG;
    frame->capacity = capacity;
    frame->count = 0;
    frame->carrier_hz = 0;
    return ESP_OK;
}

ir_frame_t *ir_frame_create(size_t capacity)
{
    size_t size = ir_frame_size(capacity);
    ir_frame_t *frame = size ? malloc(size) : NULL;
    if (frame) ir_frame_init(frame, capacity);
    return frame;
}

ir_frame_t *ir_frame_clone(const ir_frame_t *source)
{
    if (ir_frame_validate(source) != ESP_OK) return NULL;
    ir_frame_t *copy = ir_frame_create(source->count);
    if (!copy) return NULL;
    copy->count = source->count;
    copy->carrier_hz = source->carrier_hz;
    memcpy(copy->pulses_us, source->pulses_us, source->count * sizeof(uint32_t));
    return copy;
}

void ir_frame_delete(ir_frame_t *frame)
{
    free(frame);
}

esp_err_t ir_frame_validate(const ir_frame_t *frame)
{
    if (!frame || !frame->count || frame->count > frame->capacity || frame->count > IR_FRAME_MAX_PULSES) return ESP_ERR_INVALID_ARG;
    if (frame->carrier_hz && (frame->carrier_hz < IR_FRAME_MIN_CARRIER_HZ || frame->carrier_hz > IR_FRAME_MAX_CARRIER_HZ)) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < frame->count; ++i) {
        if (!frame->pulses_us[i] || frame->pulses_us[i] > IR_FRAME_MAX_PULSE_US) return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t ir_frame_push_level(ir_frame_t *frame, int level, uint32_t duration_us)
{
    if (!frame || (level != 0 && level != 1) || !duration_us) return ESP_ERR_INVALID_ARG;
    if (!frame->count && !level) return ESP_OK;
    if (frame->count && (int)((frame->count - 1) & 1U) == !level) {
        uint32_t *last = &frame->pulses_us[frame->count - 1];
        if (*last > IR_FRAME_MAX_PULSE_US - duration_us) return ESP_ERR_INVALID_SIZE;
        *last += duration_us;
        return ESP_OK;
    }
    if (frame->count >= frame->capacity) return ESP_ERR_INVALID_SIZE;
    frame->pulses_us[frame->count++] = duration_us;
    return ESP_OK;
}

esp_err_t ir_frame_push_pair(ir_frame_t *frame, uint32_t mark_us, uint32_t space_us)
{
    esp_err_t err = ir_frame_push_level(frame, 1, mark_us);
    return err == ESP_OK ? ir_frame_push_level(frame, 0, space_us) : err;
}

void ir_frame_normalize(ir_frame_t *frame, uint32_t glitch_us)
{
    if (!frame) return;
    uint32_t *p = frame->pulses_us;
    size_t w = 0;
    for (size_t r = 0; r < frame->count; ++r) {
        if (p[r] >= glitch_us) {
            p[w++] = p[r];
        } else if (w == 0) {
            r++; /* the following pulse would become a leading space */
        } else if (r + 1 < frame->count) {
            uint64_t sum = (uint64_t)p[w - 1] + p[r] + p[r + 1];
            p[w - 1] = sum > IR_FRAME_MAX_PULSE_US ? IR_FRAME_MAX_PULSE_US : (uint32_t)sum;
            r++;
        }
    }
    frame->count = w;
}

uint64_t ir_frame_duration_us(const ir_frame_t *frame)
{
    uint64_t total = 0;
    for (size_t i = 0; frame && i < frame->count; ++i) total += frame->pulses_us[i];
    return total;
}
