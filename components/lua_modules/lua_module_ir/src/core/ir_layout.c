/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ir_layout.h"

#include <string.h>

typedef struct {
    const ir_frame_t *frame;
    size_t index;
    uint32_t taken;                     /* nominal duration already matched inside the current pulse */
    uint32_t elapsed;                   /* nominal duration matched in this copy */
    bool started;
} cursor_t;

typedef struct {
    ir_frame_t *frame;
    uint32_t elapsed;
} emitter_t;

static uint32_t tolerance(uint64_t expected)
{
    uint64_t tol = expected * IR_LAYOUT_TOLERANCE_PCT / 100;
    return tol > IR_LAYOUT_TOLERANCE_MIN_US ? (uint32_t)tol : IR_LAYOUT_TOLERANCE_MIN_US;
}

static bool windows_overlap(uint32_t a, uint32_t b)
{
    return (uint64_t)a + tolerance(a) + tolerance(b) >= b && (uint64_t)b + tolerance(a) + tolerance(b) >= a;
}

static bool cursor_end(const cursor_t *c)
{
    return c->index >= c->frame->count;
}

static int cursor_level(const cursor_t *c)
{
    return (c->index & 1U) == 0;
}

static uint64_t cursor_pulse(const cursor_t *c)
{
    return c->frame->pulses_us[c->index];
}

static void cursor_advance(cursor_t *c)
{
    c->index++;
    c->taken = 0;
}

/*
 * A pulse longer than expected may be split across consecutive elements of the same level.
 * The whole pulse is compared with the sum of its elements so errors do not pile up on the last one.
 */
static bool take(cursor_t *c, int level, uint32_t expected, bool split)
{
    if (!cursor_end(c) && cursor_level(c) != level) return level == 0 && !c->started;
    c->started = true;
    c->elapsed += expected;
    if (cursor_end(c)) return level == 0;
    uint64_t pulse = cursor_pulse(c);
    uint64_t total = (uint64_t)c->taken + expected;
    uint32_t tol = tolerance(total);
    if (pulse + tol >= total && pulse <= total + tol) {
        cursor_advance(c);
        return true;
    }
    if (split && pulse > total + tol) {
        c->taken = (uint32_t)total;
        return true;
    }
    return false;
}

static bool take_period(cursor_t *c, uint32_t period)
{
    if (c->elapsed >= period) return false;
    uint32_t gap = period - c->elapsed;
    c->elapsed = period;
    if (cursor_end(c)) return true;
    uint64_t total = (uint64_t)c->taken + gap;
    if (cursor_level(c) != 0 || cursor_pulse(c) + tolerance(total) < total) return false;
    cursor_advance(c);
    return true;
}

static int take_pair_bit(cursor_t *c, const ir_layout_t *layout)
{
    cursor_t start = *c;
    for (int split = 0; split < 2; ++split) {
        for (int bit = 0; bit < 2; ++bit) {
            const ir_symbol_t *symbol = &layout->symbols[bit];
            *c = start;
            if (take(c, 1, symbol->mark_us, false) && take(c, 0, symbol->space_us, split)) return bit;
        }
    }
    return -1;
}

static int take_biphase_bit(cursor_t *c, const ir_layout_t *layout)
{
    if (cursor_end(c)) return -1;
    int first = cursor_level(c);
    if (!take(c, first, layout->half_us, true) || !take(c, !first, layout->half_us, true)) return -1;
    return first == layout->one_level;
}

static esp_err_t decode_at(const ir_layout_t *layout, const ir_frame_t *frame, size_t offset, ir_bits_t *bits, size_t *next)
{
    cursor_t c = {.frame = frame, .index = offset};
    memset(bits, 0, sizeof(*bits));
    for (uint16_t i = 0; i < layout->step_count; ++i) {
        const ir_step_t *step = &layout->steps[i];
        bool ok = true;
        switch (step->kind) {
        case IR_STEP_MARK:
            ok = take(&c, 1, step->value_us, true);
            break;
        case IR_STEP_SPACE:
            ok = take(&c, 0, step->value_us, true);
            break;
        case IR_STEP_PERIOD:
            ok = take_period(&c, step->value_us);
            break;
        case IR_STEP_BITS:
            for (uint16_t n = 0; ok && n < step->count; ++n) {
                int bit = layout->coding == IR_CODING_PAIR ? take_pair_bit(&c, layout) : take_biphase_bit(&c, layout);
                ok = bit >= 0 && bits->count < IR_BITS_MAX;
                if (ok && bit) bits->data[bits->count >> 3] |= (uint8_t)(1U << (bits->count & 7U));
                bits->count++;
            }
            break;
        default:
            ok = false;
            break;
        }
        if (!ok) return ESP_ERR_NOT_FOUND;
    }
    *next = c.index + (!cursor_end(&c) && !cursor_level(&c));
    return ESP_OK;
}

esp_err_t ir_layout_decode(const ir_layout_t *layout, const ir_frame_t *frame, size_t offset, ir_bits_t *bits, size_t *next)
{
    if (!layout || !layout->steps || !frame || !bits || !next) return ESP_ERR_INVALID_ARG;
    offset += offset & 1U;
    for (unsigned tries = 0; tries < IR_LAYOUT_SYNC_TRIES && offset < frame->count; ++tries, offset += 2) {
        if (decode_at(layout, frame, offset, bits, next) == ESP_OK) return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t emit(emitter_t *e, int level, uint32_t duration_us)
{
    if (!e->frame->count && !level) return ESP_OK;
    esp_err_t err = ir_frame_push_level(e->frame, level, duration_us);
    if (err == ESP_OK) e->elapsed += duration_us;
    return err;
}

static esp_err_t emit_bit(emitter_t *e, const ir_layout_t *layout, bool bit)
{
    if (layout->coding == IR_CODING_PAIR) {
        const ir_symbol_t *symbol = &layout->symbols[bit];
        esp_err_t err = emit(e, 1, symbol->mark_us);
        return err == ESP_OK ? emit(e, 0, symbol->space_us) : err;
    }
    int first = bit ? layout->one_level : !layout->one_level;
    esp_err_t err = emit(e, first, layout->half_us);
    return err == ESP_OK ? emit(e, !first, layout->half_us) : err;
}

esp_err_t ir_layout_encode(const ir_layout_t *layout, const ir_bits_t *bits, ir_frame_t *frame)
{
    if (!layout || !layout->steps || !bits || !frame) return ESP_ERR_INVALID_ARG;
    emitter_t e = {.frame = frame};
    uint16_t bit = 0;
    esp_err_t err = ESP_OK;
    for (uint16_t i = 0; err == ESP_OK && i < layout->step_count; ++i) {
        const ir_step_t *step = &layout->steps[i];
        switch (step->kind) {
        case IR_STEP_MARK:
            err = emit(&e, 1, step->value_us);
            break;
        case IR_STEP_SPACE:
            err = emit(&e, 0, step->value_us);
            break;
        case IR_STEP_PERIOD:
            err = e.elapsed < step->value_us ? emit(&e, 0, step->value_us - e.elapsed) : ESP_ERR_INVALID_SIZE;
            break;
        case IR_STEP_BITS:
            for (uint16_t n = 0; err == ESP_OK && n < step->count; ++n) {
                err = bit < bits->count ? emit_bit(&e, layout, ir_bits_get(bits, bit++)) : ESP_ERR_INVALID_SIZE;
            }
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
        }
    }
    return err == ESP_OK && bit != bits->count ? ESP_ERR_INVALID_SIZE : err;
}

uint16_t ir_layout_bit_count(const ir_layout_t *layout)
{
    uint32_t count = 0;
    for (uint16_t i = 0; layout && i < layout->step_count; ++i) {
        if (layout->steps[i].kind == IR_STEP_BITS) count += layout->steps[i].count;
    }
    return count > UINT16_MAX ? UINT16_MAX : (uint16_t)count;
}

size_t ir_layout_max_pulses(const ir_layout_t *layout)
{
    size_t count = 0;
    for (uint16_t i = 0; layout && i < layout->step_count; ++i) {
        count += layout->steps[i].kind == IR_STEP_BITS ? 2U * layout->steps[i].count : 1U;
    }
    return count;
}

static uint32_t max_bit_us(const ir_layout_t *layout)
{
    if (layout->coding == IR_CODING_BIPHASE) return 2U * layout->half_us;
    uint32_t zero = (uint32_t)layout->symbols[0].mark_us + layout->symbols[0].space_us;
    uint32_t one = (uint32_t)layout->symbols[1].mark_us + layout->symbols[1].space_us;
    return zero > one ? zero : one;
}

uint32_t ir_layout_max_duration_us(const ir_layout_t *layout)
{
    uint64_t elapsed = 0;
    for (uint16_t i = 0; layout && i < layout->step_count; ++i) {
        const ir_step_t *step = &layout->steps[i];
        if (step->kind == IR_STEP_BITS) {
            elapsed += (uint64_t)step->count * max_bit_us(layout);
        } else if (step->kind == IR_STEP_PERIOD) {
            if (elapsed < step->value_us) elapsed = step->value_us;
        } else {
            elapsed += step->value_us;
        }
    }
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

esp_err_t ir_layout_validate(const ir_layout_t *layout, bool decodable)
{
    if (!layout || !layout->steps || !layout->step_count || layout->coding > IR_CODING_BIPHASE) return ESP_ERR_INVALID_ARG;
    bool pair = layout->coding == IR_CODING_PAIR;
    const ir_symbol_t *s = layout->symbols;
    bool bad_symbols = pair ? (!s[0].mark_us || !s[0].space_us || !s[1].mark_us || !s[1].space_us) : (!layout->half_us || layout->one_level > 1);
    if (bad_symbols && ir_layout_bit_count(layout)) return ESP_ERR_INVALID_ARG;
    bool marks_apart = !windows_overlap(s[0].mark_us, s[1].mark_us);
    uint64_t elapsed = 0;
    uint32_t bits = 0;
    for (uint16_t i = 0; i < layout->step_count; ++i) {
        const ir_step_t *step = &layout->steps[i];
        switch (step->kind) {
        case IR_STEP_MARK:
        case IR_STEP_SPACE:
            if (!step->value_us || step->value_us > IR_FRAME_MAX_PULSE_US) return ESP_ERR_INVALID_ARG;
            elapsed += step->value_us;
            break;
        case IR_STEP_BITS:
            if (!step->count) return ESP_ERR_INVALID_ARG;
            /* With equal marks the last bit needs a following mark to show its space. */
            if (decodable && pair && !marks_apart && (i + 1 >= layout->step_count || layout->steps[i + 1].kind != IR_STEP_MARK)) return ESP_ERR_INVALID_ARG;
            bits += step->count;
            elapsed += (uint64_t)step->count * max_bit_us(layout);
            break;
        case IR_STEP_PERIOD:
            if (step->value_us <= elapsed || step->value_us > IR_FRAME_MAX_PULSE_US) return ESP_ERR_INVALID_ARG;
            elapsed = step->value_us;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (bits > IR_BITS_MAX) return ESP_ERR_INVALID_SIZE;
    if (decodable && bits) {
        bool separable = pair ? marks_apart || !windows_overlap(s[0].space_us, s[1].space_us) : !windows_overlap(layout->half_us, 2U * layout->half_us);
        if (!separable) return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
