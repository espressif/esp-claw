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

/* Nearest-symbol decoding needs each nominal duration outside the window of the other. */
static bool durations_apart(uint32_t a, uint32_t b)
{
    uint32_t high = a > b ? a : b;
    uint32_t low = a > b ? b : a;
    return high - low > tolerance(high);
}

static unsigned symbol_bits(const ir_layout_t *layout)
{
    return layout->coding == IR_CODING_PAIR && layout->symbol_bits ? layout->symbol_bits : 1U;
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

static uint64_t pulse_error(const cursor_t *c, uint32_t expected)
{
    if (cursor_end(c)) return 0;
    uint64_t pulse = cursor_pulse(c);
    uint64_t total = (uint64_t)c->taken + expected;
    return pulse > total ? pulse - total : total - pulse;
}

/* Exact matches pick the nearest symbol; a space running into a following gap takes the first symbol that fits. */
static int take_pair_symbol(cursor_t *c, const ir_layout_t *layout)
{
    const cursor_t start = *c;
    cursor_t best = start;
    uint64_t best_error = UINT64_MAX;
    int value = -1;
    unsigned count = 1U << symbol_bits(layout);
    for (int split = 0; split < 2 && value < 0; ++split) {
        for (unsigned v = 0; v < count; ++v) {
            const ir_symbol_t *symbol = &layout->symbols[v];
            *c = start;
            uint64_t error = pulse_error(c, symbol->mark_us);
            if (!take(c, 1, symbol->mark_us, false)) continue;
            error += pulse_error(c, symbol->space_us);
            if (!take(c, 0, symbol->space_us, split) || error >= best_error) continue;
            best = *c;
            best_error = split ? 0 : error;
            value = (int)v;
        }
    }
    *c = best;
    return value;
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
    unsigned width = symbol_bits(layout);
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
            for (uint16_t n = 0; ok && n < step->count; n += width) {
                int value = layout->coding == IR_CODING_PAIR ? take_pair_symbol(&c, layout) : take_biphase_bit(&c, layout);
                ok = value >= 0 && bits->count + width <= IR_BITS_MAX;
                for (unsigned b = width; ok && b-- > 0; bits->count++) {
                    if ((value >> b) & 1) bits->data[bits->count >> 3] |= (uint8_t)(1U << (bits->count & 7U));
                }
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

static esp_err_t emit_symbol(emitter_t *e, const ir_layout_t *layout, unsigned value)
{
    if (layout->coding == IR_CODING_PAIR) {
        const ir_symbol_t *symbol = &layout->symbols[value];
        esp_err_t err = emit(e, 1, symbol->mark_us);
        return err == ESP_OK ? emit(e, 0, symbol->space_us) : err;
    }
    int first = value ? layout->one_level : !layout->one_level;
    esp_err_t err = emit(e, first, layout->half_us);
    return err == ESP_OK ? emit(e, !first, layout->half_us) : err;
}

esp_err_t ir_layout_encode(const ir_layout_t *layout, const ir_bits_t *bits, ir_frame_t *frame)
{
    if (!layout || !layout->steps || !bits || !frame) return ESP_ERR_INVALID_ARG;
    emitter_t e = {.frame = frame};
    unsigned width = symbol_bits(layout);
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
            for (uint16_t n = 0; err == ESP_OK && n < step->count; n += width) {
                if (bit + width > bits->count) {
                    err = ESP_ERR_INVALID_SIZE;
                    break;
                }
                unsigned value = 0;
                for (unsigned b = 0; b < width; ++b) value = value << 1 | ir_bits_get(bits, bit++);
                err = emit_symbol(&e, layout, value);
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
        count += layout->steps[i].kind == IR_STEP_BITS ? 2U * layout->steps[i].count / symbol_bits(layout) : 1U;
    }
    return count;
}

static uint32_t max_symbol_us(const ir_layout_t *layout)
{
    if (layout->coding == IR_CODING_BIPHASE) return 2U * layout->half_us;
    uint32_t longest = 0;
    for (unsigned v = 0; v < 1U << symbol_bits(layout); ++v) {
        uint32_t us = (uint32_t)layout->symbols[v].mark_us + layout->symbols[v].space_us;
        if (us > longest) longest = us;
    }
    return longest;
}

uint32_t ir_layout_max_duration_us(const ir_layout_t *layout)
{
    uint64_t elapsed = 0;
    for (uint16_t i = 0; layout && i < layout->step_count; ++i) {
        const ir_step_t *step = &layout->steps[i];
        if (step->kind == IR_STEP_BITS) {
            elapsed += (uint64_t)step->count / symbol_bits(layout) * max_symbol_us(layout);
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
    if (layout->symbol_bits > (pair ? IR_LAYOUT_SYMBOL_BITS_MAX : 1)) return ESP_ERR_INVALID_ARG;
    const ir_symbol_t *s = layout->symbols;
    unsigned width = symbol_bits(layout);
    unsigned count = 1U << width;
    bool bad_symbols = !pair && (!layout->half_us || layout->one_level > 1);
    bool marks_apart = true;
    bool separable = true;
    for (unsigned i = 0; pair && i < count; ++i) {
        bad_symbols |= !s[i].mark_us || !s[i].space_us;
        for (unsigned j = i + 1; j < count; ++j) {
            bool mark_apart = durations_apart(s[i].mark_us, s[j].mark_us);
            marks_apart &= mark_apart;
            separable &= mark_apart || durations_apart(s[i].space_us, s[j].space_us);
        }
    }
    if (!pair) separable = !windows_overlap(layout->half_us, 2U * layout->half_us);
    if (bad_symbols && ir_layout_bit_count(layout)) return ESP_ERR_INVALID_ARG;
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
            if (!step->count || step->count % width) return ESP_ERR_INVALID_ARG;
            /* With equal marks the last symbol needs a following mark to show its space. */
            if (decodable && pair && !marks_apart && (i + 1 >= layout->step_count || layout->steps[i + 1].kind != IR_STEP_MARK)) return ESP_ERR_INVALID_ARG;
            bits += step->count;
            elapsed += (uint64_t)step->count / width * max_symbol_us(layout);
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
    if (decodable && bits && !separable) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}
