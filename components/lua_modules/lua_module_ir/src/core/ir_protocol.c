/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ir_protocol.h"

#include <string.h>

#define FIELD_MAX_WIDTH 32
#define US_PER_MS 1000U

static uint8_t copies_of(const ir_message_t *m)
{
    return m->copies ? m->copies : 1;
}

static uint8_t low_mask(uint8_t bits)
{
    return bits >= 8 ? 0xFF : (uint8_t)((1U << bits) - 1U);
}

static uint8_t byte_width(const ir_message_t *m, uint8_t index)
{
    for (uint8_t i = 0; i < m->width_count; ++i) {
        if (m->widths[i].byte == index) return m->widths[i].bits;
    }
    return 8;
}

static uint16_t payload_bits(const ir_message_t *m)
{
    uint16_t bits = 0;
    for (uint8_t i = 0; i < m->length; ++i) bits += byte_width(m, i);
    return bits;
}

static void pack(const ir_message_t *m, const uint8_t *payload, ir_bits_t *bits)
{
    memset(bits, 0, sizeof(*bits));
    for (uint8_t i = 0; i < m->length; ++i) {
        uint8_t width = byte_width(m, i);
        for (uint8_t b = 0; b < width; ++b, ++bits->count) {
            uint8_t pos = m->msb_first ? width - 1 - b : b;
            if ((payload[i] >> pos) & 1U) bits->data[bits->count >> 3] |= (uint8_t)(1U << (bits->count & 7U));
        }
    }
}

static void unpack(const ir_message_t *m, const ir_bits_t *bits, uint8_t *payload)
{
    uint16_t n = 0;
    for (uint8_t i = 0; i < m->length; ++i) {
        uint8_t width = byte_width(m, i);
        payload[i] = 0;
        for (uint8_t b = 0; b < width; ++b) {
            uint8_t pos = m->msb_first ? width - 1 - b : b;
            if (ir_bits_get(bits, n++)) payload[i] |= (uint8_t)(1U << pos);
        }
    }
}

static uint8_t segment_mask(const ir_bit_segment_t *s)
{
    return (uint8_t)(low_mask(s->width) << s->shift);
}

static uint8_t field_width(const ir_bitfield_t *f)
{
    uint8_t width = 0;
    for (uint8_t i = 0; i < f->segment_count; ++i) width += f->segments[i].width;
    return width;
}

static int32_t field_step(const ir_bitfield_t *f)
{
    return f->step ? f->step : 1;
}

static uint32_t field_read(const ir_bitfield_t *f, const uint8_t *payload)
{
    uint32_t raw = 0;
    uint8_t shift = 0;
    for (uint8_t i = 0; i < f->segment_count; ++i) {
        const ir_bit_segment_t *s = &f->segments[i];
        raw |= (uint32_t)((payload[s->byte] >> s->shift) & low_mask(s->width)) << shift;
        shift += s->width;
    }
    return raw;
}

static void field_write(const ir_bitfield_t *f, uint8_t *payload, uint32_t raw)
{
    for (uint8_t i = 0; i < f->segment_count; ++i) {
        const ir_bit_segment_t *s = &f->segments[i];
        payload[s->byte] = (uint8_t)((payload[s->byte] & ~segment_mask(s)) | ((raw & low_mask(s->width)) << s->shift));
        raw >>= s->width;
    }
}

static esp_err_t field_to_raw(const ir_bitfield_t *f, int32_t value, uint32_t *raw)
{
    int64_t offset = (int64_t)value - f->base;
    int32_t step = field_step(f);
    if (offset < 0 || offset % step) return ESP_ERR_NOT_SUPPORTED;
    uint64_t index = (uint64_t)(offset / step);
    if (f->value_count) {
        if (index >= f->value_count || f->raw[index] == IR_RAW_NONE) return ESP_ERR_NOT_SUPPORTED;
        *raw = f->raw[index];
        return ESP_OK;
    }
    uint8_t width = field_width(f);
    if (width < FIELD_MAX_WIDTH ? index >> width : index > UINT32_MAX) return ESP_ERR_NOT_SUPPORTED;
    *raw = (uint32_t)index;
    return ESP_OK;
}

static esp_err_t field_from_raw(const ir_bitfield_t *f, uint32_t raw, int32_t *value)
{
    int64_t index = raw;
    if (f->value_count) {
        for (index = 0; index < f->value_count && f->raw[index] != raw; ++index) {}
        if (index == f->value_count) return ESP_ERR_NOT_SUPPORTED;
    }
    int64_t result = f->base + index * field_step(f);
    if (result < INT32_MIN || result > INT32_MAX) return ESP_ERR_NOT_SUPPORTED;
    *value = (int32_t)result;
    return ESP_OK;
}

static bool check_target_nibble(const ir_check_t *c)
{
    return (c->flags & IR_CHECK_NIBBLE) && !(c->flags & IR_CHECK_BYTE_TARGET);
}

static uint8_t check_target_count(const ir_check_t *c)
{
    return c->op == IR_CHECK_COPY ? c->count : 1;
}

static uint8_t check_target_mask(const ir_check_t *c, uint8_t byte)
{
    uint8_t mask = 0;
    bool nibble = check_target_nibble(c);
    for (uint8_t k = 0; k < check_target_count(c); ++k) {
        unsigned unit = (unsigned)c->target + k;
        if (!nibble && unit == byte) mask = 0xFF;
        if (nibble && unit >> 1 == byte) mask |= (unit & 1U) ? 0x0F : 0xF0;
    }
    return mask;
}

/* Bits decode does not compare against the template. */
static uint8_t free_mask(const ir_message_t *m, uint8_t byte)
{
    uint8_t mask = m->ignore ? m->ignore[byte] : 0;
    for (uint8_t i = 0; i < m->field_count; ++i) {
        for (uint8_t s = 0; s < m->fields[i].segment_count; ++s) {
            if (m->fields[i].segments[s].byte == byte) mask |= segment_mask(&m->fields[i].segments[s]);
        }
    }
    for (uint8_t i = 0; i < m->check_count; ++i) mask |= check_target_mask(&m->checks[i], byte);
    return mask;
}

static uint8_t unit_get(const uint8_t *payload, unsigned unit, bool nibble)
{
    if (!nibble) return payload[unit];
    return (unit & 1U) ? payload[unit >> 1] & 0x0F : payload[unit >> 1] >> 4;
}

static void unit_set(uint8_t *payload, unsigned unit, bool nibble, uint8_t value)
{
    if (!nibble) {
        payload[unit] = value;
        return;
    }
    uint8_t *b = &payload[unit >> 1];
    *b = (unit & 1U) ? (uint8_t)((*b & 0xF0) | (value & 0x0F)) : (uint8_t)((*b & 0x0F) | (value << 4));
}

static void check_apply(const ir_check_t *c, uint8_t *payload)
{
    bool nibble = c->flags & IR_CHECK_NIBBLE;
    bool target_nibble = check_target_nibble(c);
    uint8_t invert = (c->flags & IR_CHECK_INVERT) ? 0xFF : 0;
    uint32_t acc = c->bias;
    for (uint8_t i = 0; i < c->count; ++i) {
        uint8_t value = unit_get(payload, c->units ? c->units[i] : (unsigned)c->first + i, nibble);
        if (c->op == IR_CHECK_COPY) {
            unit_set(payload, (unsigned)c->target + i, target_nibble, value ^ invert);
        } else {
            acc = c->op == IR_CHECK_SUM ? acc + value : acc ^ value;
        }
    }
    if (c->op != IR_CHECK_COPY) unit_set(payload, c->target, target_nibble, (uint8_t)acc ^ invert);
}

static void apply_checks(const ir_message_t *m, uint8_t *payload)
{
    for (uint8_t i = 0; i < m->check_count; ++i) check_apply(&m->checks[i], payload);
    for (uint8_t i = 0; i < m->length; ++i) payload[i] &= low_mask(byte_width(m, i));
}

static esp_err_t build_payload(const ir_message_t *m, const ir_values_t *values, uint8_t *payload)
{
    if (m->template_bytes) {
        memcpy(payload, m->template_bytes, m->length);
    } else {
        memset(payload, 0, m->length);
    }
    for (uint8_t i = 0; i < m->field_count; ++i) {
        const ir_bitfield_t *f = &m->fields[i];
        uint32_t raw;
        if (!ir_values_has(values, (ir_field_id_t)f->id)) return ESP_ERR_INVALID_ARG;
        esp_err_t err = field_to_raw(f, values->value[f->id], &raw);
        if (err != ESP_OK) return err;
        field_write(f, payload, raw);
    }
    apply_checks(m, payload);
    return ESP_OK;
}

static esp_err_t encode_message(const ir_message_t *m, const ir_values_t *values, ir_frame_t *frame)
{
    uint8_t payload[IR_PAYLOAD_MAX];
    ir_bits_t bits;
    esp_err_t err = build_payload(m, values, payload);
    if (err != ESP_OK) return err;
    pack(m, payload, &bits);
    for (uint8_t c = 0; err == ESP_OK && c < copies_of(m); ++c) err = ir_layout_encode(m->layout, &bits, frame);
    return err;
}

static esp_err_t decode_message(const ir_message_t *m, const ir_bits_t *bits, ir_values_t *values)
{
    uint8_t payload[IR_PAYLOAD_MAX];
    uint8_t expect[IR_PAYLOAD_MAX];
    if (bits->count != payload_bits(m)) return ESP_ERR_NOT_FOUND;
    unpack(m, bits, payload);
    for (uint8_t i = 0; i < m->length; ++i) {
        uint8_t template_byte = m->template_bytes ? m->template_bytes[i] : 0;
        if ((payload[i] ^ template_byte) & low_mask(byte_width(m, i)) & ~free_mask(m, i)) return ESP_ERR_NOT_FOUND;
    }
    memcpy(expect, payload, m->length);
    apply_checks(m, expect);
    for (uint8_t i = 0; i < m->length; ++i) {
        uint8_t ignore = m->ignore ? m->ignore[i] : 0;
        if ((payload[i] ^ expect[i]) & ~ignore) return ESP_ERR_INVALID_CRC;
    }
    memset(values, 0, sizeof(*values));
    if (m->implied) *values = *m->implied;
    for (uint8_t i = 0; i < m->field_count; ++i) {
        const ir_bitfield_t *f = &m->fields[i];
        int32_t value;
        if (field_from_raw(f, field_read(f, payload), &value) != ESP_OK) return ESP_ERR_NOT_SUPPORTED;
        ir_values_set(values, (ir_field_id_t)f->id, value);
    }
    return ESP_OK;
}

static uint16_t constrained_bits(const ir_message_t *m)
{
    uint16_t bits = 0;
    for (uint8_t i = 0; i < m->length; ++i) {
        uint8_t field_or_ignore = free_mask(m, i);
        for (uint8_t c = 0; c < m->check_count; ++c) field_or_ignore &= (uint8_t)~check_target_mask(&m->checks[c], i);
        bits += (uint16_t)__builtin_popcount(low_mask(byte_width(m, i)) & ~field_or_ignore);
    }
    return bits;
}

/* Later decode stages win: a checksum failure says more than no match. */
static esp_err_t stronger_error(esp_err_t current, esp_err_t err)
{
    if (err == ESP_ERR_NOT_SUPPORTED || current == ESP_ERR_NOT_SUPPORTED) return ESP_ERR_NOT_SUPPORTED;
    if (err == ESP_ERR_INVALID_CRC || current == ESP_ERR_INVALID_CRC) return ESP_ERR_INVALID_CRC;
    return current;
}

static uint8_t count_repeats(const ir_message_t *m, const ir_frame_t *frame, const ir_bits_t *same, size_t *next)
{
    ir_bits_t bits;
    ir_values_t values;
    uint8_t count = 0;
    size_t at;
    while (count < UINT8_MAX && *next < frame->count && ir_layout_decode(m->layout, frame, *next, &bits, &at) == ESP_OK && at > *next) {
        bool match = same ? bits.count == same->count && !memcmp(bits.data, same->data, (bits.count + 7U) / 8U) : decode_message(m, &bits, &values) == ESP_OK;
        if (!match) break;
        *next = at;
        count++;
    }
    return count;
}

uint32_t ir_message_fields(const ir_message_t *message)
{
    uint32_t mask = 0;
    for (uint8_t i = 0; message && i < message->field_count; ++i) mask |= IR_FIELD_BIT(message->fields[i].id);
    return mask;
}

uint32_t ir_protocol_fields(const ir_protocol_t *protocol)
{
    uint32_t mask = 0;
    for (uint8_t i = 0; protocol && i < protocol->message_count; ++i) {
        mask |= ir_message_fields(&protocol->messages[i]);
        if (protocol->messages[i].implied) mask |= protocol->messages[i].implied->present;
    }
    return mask;
}

bool ir_protocol_get_limit(const ir_protocol_t *protocol, ir_field_id_t field, int32_t mode, ir_limit_t *limit)
{
    const ir_protocol_meta_t *meta = protocol ? protocol->meta : NULL;
    if (!meta || (unsigned)field >= IR_FIELD_COUNT || !limit) return false;
    if (mode >= 0 && mode < IR_MODE_COUNT && (meta->mode_limit_fields[mode] & IR_FIELD_BIT(field))) {
        *limit = meta->mode_limits[mode][field];
        return true;
    }
    if (!(meta->limit_fields & IR_FIELD_BIT(field))) return false;
    *limit = meta->limits[field];
    return true;
}

static bool limit_allows(ir_field_id_t field, const ir_limit_t *limit, int32_t value)
{
    ir_field_type_t type = ir_field_info(field)->type;
    if (type == IR_FIELD_TYPE_BOOL || type == IR_FIELD_TYPE_ENUM) return value >= 0 && value < IR_FIELD_ENUM_MAX && ((limit->mask >> value) & 1U);
    return value >= limit->min && value <= limit->max && ((int64_t)value - limit->min) % limit->step == 0;
}

esp_err_t ir_protocol_check(const ir_protocol_t *protocol, const ir_values_t *values)
{
    if (!protocol || !values) return ESP_ERR_INVALID_ARG;
    int32_t mode = ir_values_has(values, IR_FIELD_MODE) ? values->value[IR_FIELD_MODE] : -1;
    for (unsigned i = 0; protocol->meta && i < IR_FIELD_COUNT; ++i) {
        ir_limit_t limit;
        if (ir_values_has(values, (ir_field_id_t)i) && ir_protocol_get_limit(protocol, (ir_field_id_t)i, mode, &limit) && !ir_limit_unused(&limit) && !limit_allows((ir_field_id_t)i, &limit, values->value[i])) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    return ESP_OK;
}

const ir_message_t *ir_protocol_select(const ir_protocol_t *protocol, const ir_values_t *values)
{
    const ir_message_t *best = NULL;
    int best_count = -1;
    for (uint8_t i = 0; protocol && values && i < protocol->message_count; ++i) {
        const ir_message_t *m = &protocol->messages[i];
        int count = m->implied ? __builtin_popcount(m->implied->present) : 0;
        if (count > best_count && (!m->implied || ir_values_contains(values, m->implied))) {
            best = m;
            best_count = count;
        }
    }
    return best;
}

size_t ir_protocol_max_pulses(const ir_protocol_t *protocol, uint32_t holds)
{
    size_t most = 0;
    for (uint8_t i = 0; protocol && i < protocol->message_count; ++i) {
        size_t pulses = ir_layout_max_pulses(protocol->messages[i].layout) * copies_of(&protocol->messages[i]);
        if (pulses > most) most = pulses;
    }
    size_t hold = protocol && protocol->hold ? ir_layout_max_pulses(protocol->hold->layout) * copies_of(protocol->hold) : most;
    return most + holds * hold;
}

uint32_t ir_protocol_hold_count(const ir_protocol_t *protocol, const ir_values_t *values, uint32_t hold_ms)
{
    const ir_message_t *hold = protocol && protocol->hold ? protocol->hold : ir_protocol_select(protocol, values);
    if (!hold_ms || !hold) return 0;
    uint64_t unit_us = (uint64_t)ir_layout_max_duration_us(hold->layout) * copies_of(hold);
    uint64_t holds = unit_us ? (uint64_t)hold_ms * US_PER_MS / unit_us : 0;
    size_t base = ir_protocol_max_pulses(protocol, 0);
    size_t step = ir_protocol_max_pulses(protocol, 1) - base;
    size_t limit = step && base < IR_FRAME_MAX_PULSES ? (IR_FRAME_MAX_PULSES - base) / step : 0;
    return holds > limit ? (uint32_t)limit : (uint32_t)holds;
}

esp_err_t ir_protocol_encode(const ir_protocol_t *protocol, const ir_values_t *values, uint32_t holds, ir_frame_t *frame)
{
    if (!protocol || !values || !frame) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ir_protocol_check(protocol, values);
    if (err != ESP_OK) return err;
    const ir_message_t *m = ir_protocol_select(protocol, values);
    if (!m) return ESP_ERR_NOT_SUPPORTED;
    frame->count = 0;
    frame->carrier_hz = protocol->carrier_hz;
    err = encode_message(m, values, frame);
    const ir_message_t *hold = protocol->hold ? protocol->hold : m;
    for (uint32_t i = 0; err == ESP_OK && i < holds; ++i) err = encode_message(hold, values, frame);
    return err;
}

esp_err_t ir_protocol_decode(const ir_protocol_t *protocol, const ir_frame_t *frame, size_t offset, ir_decode_result_t *result)
{
    if (!protocol || !result || ir_frame_validate(frame) != ESP_OK) return ESP_ERR_INVALID_ARG;
    if (!(protocol->flags & IR_PROTOCOL_DECODE)) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t best = ESP_ERR_NOT_FOUND;
    for (uint8_t i = 0; i < protocol->message_count; ++i) {
        const ir_message_t *m = &protocol->messages[i];
        ir_bits_t bits;
        size_t next;
        if (ir_layout_decode(m->layout, frame, offset, &bits, &next) != ESP_OK) continue;
        esp_err_t err = decode_message(m, &bits, &result->values);
        if (err != ESP_OK) {
            best = stronger_error(best, err);
            continue;
        }
        result->message = m;
        result->next = next;
        result->copies = 1 + count_repeats(m, frame, &bits, &result->next);
        result->holds = protocol->hold ? count_repeats(protocol->hold, frame, NULL, &result->next) : 0;
        return ESP_OK;
    }
    return best;
}

static esp_err_t validate_field(const ir_message_t *m, const ir_bitfield_t *f, bool decodable, uint8_t *used)
{
    if (f->id >= IR_FIELD_COUNT || !f->segment_count || !f->segments || f->step < 0 || (f->value_count && !f->raw)) return ESP_ERR_INVALID_ARG;
    uint8_t width = field_width(f);
    if (width > FIELD_MAX_WIDTH) return ESP_ERR_INVALID_ARG;
    for (uint8_t i = 0; i < f->segment_count; ++i) {
        const ir_bit_segment_t *s = &f->segments[i];
        if (s->byte >= m->length || !s->width || s->shift + s->width > byte_width(m, s->byte)) return ESP_ERR_INVALID_ARG;
        if (used[s->byte] & segment_mask(s)) return ESP_ERR_INVALID_ARG;
        used[s->byte] |= segment_mask(s);
    }
    for (uint8_t i = 0; i < f->value_count; ++i) {
        uint32_t raw = f->raw[i];
        if (raw == IR_RAW_NONE) continue;
        if (width < FIELD_MAX_WIDTH && raw >> width) return ESP_ERR_INVALID_ARG;
        for (uint8_t j = 0; decodable && j < i; ++j) {
            if (f->raw[j] == raw) return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static esp_err_t validate_check(const ir_message_t *m, const ir_check_t *c, uint8_t *used)
{
    if (c->op > IR_CHECK_XOR || !c->count) return ESP_ERR_INVALID_ARG;
    unsigned source_units = (c->flags & IR_CHECK_NIBBLE) ? 2U * m->length : m->length;
    unsigned target_units = check_target_nibble(c) ? 2U * m->length : m->length;
    for (uint8_t i = 0; i < c->count; ++i) {
        if ((c->units ? c->units[i] : (unsigned)c->first + i) >= source_units) return ESP_ERR_INVALID_ARG;
    }
    if ((unsigned)c->target + check_target_count(c) > target_units) return ESP_ERR_INVALID_ARG;
    for (uint8_t i = 0; i < m->length; ++i) {
        uint8_t mask = check_target_mask(c, i) & low_mask(byte_width(m, i));
        if (used[i] & mask) return ESP_ERR_INVALID_ARG;
        used[i] |= mask;
    }
    return ESP_OK;
}

static esp_err_t validate_message(const ir_message_t *m, bool decodable)
{
    if (!m->layout || m->length > IR_PAYLOAD_MAX || (m->width_count && !m->widths) || (m->field_count && !m->fields) || (m->check_count && !m->checks)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ir_layout_validate(m->layout, decodable);
    if (err != ESP_OK) return err;
    for (uint8_t i = 0; i < m->width_count; ++i) {
        if (m->widths[i].byte >= m->length || !m->widths[i].bits || m->widths[i].bits > 8) return ESP_ERR_INVALID_ARG;
    }
    if (payload_bits(m) != ir_layout_bit_count(m->layout)) return ESP_ERR_INVALID_SIZE;
    uint8_t used[IR_PAYLOAD_MAX];
    for (uint8_t i = 0; i < m->length; ++i) used[i] = m->ignore ? m->ignore[i] : 0;
    for (uint8_t i = 0; err == ESP_OK && i < m->field_count; ++i) err = validate_field(m, &m->fields[i], decodable, used);
    for (uint8_t i = 0; err == ESP_OK && i < m->check_count; ++i) err = validate_check(m, &m->checks[i], used);
    return err;
}

esp_err_t ir_protocol_validate(const ir_protocol_t *protocol)
{
    if (!protocol || !protocol->id || !protocol->messages || !protocol->message_count) return ESP_ERR_INVALID_ARG;
    if (protocol->carrier_hz < IR_FRAME_MIN_CARRIER_HZ || protocol->carrier_hz > IR_FRAME_MAX_CARRIER_HZ) return ESP_ERR_INVALID_ARG;
    bool decodable = protocol->flags & IR_PROTOCOL_DECODE;
    for (uint8_t i = 0; i < protocol->message_count; ++i) {
        const ir_message_t *m = &protocol->messages[i];
        esp_err_t err = validate_message(m, decodable);
        if (err != ESP_OK) return err;
        if (decodable && !(protocol->flags & IR_PROTOCOL_UNCHECKED) && constrained_bits(m) < IR_PROTOCOL_MIN_CONSTRAINED_BITS) return ESP_ERR_INVALID_ARG;
    }
    return protocol->hold ? validate_message(protocol->hold, decodable) : ESP_OK;
}
