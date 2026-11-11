/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ir_protocol_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#define ARENA_CHUNK_SIZE 2048
#define DECI_SCALE 10
#define DECI_EPSILON 1e-6
#define LIST_MAX UINT8_MAX

#define TRY(expr) do { esp_err_t err_ = (expr); if (err_ != ESP_OK) return err_; } while (0)

static const char *TAG = "ir_protocol_json";

typedef struct arena_chunk {
    struct arena_chunk *next;
    size_t used;
    size_t size;
    uint8_t data[];
} arena_chunk_t;

typedef struct {
    ir_protocol_t protocol;             /* first member, so ir_protocol_free() can recover the owner */
    ir_protocol_meta_t meta;
    arena_chunk_t *arena;               /* owns every parsed string and table */
} json_protocol_t;

static void *arena_alloc(json_protocol_t *p, size_t size)
{
    size = (size + 7U) & ~(size_t)7U;
    arena_chunk_t *chunk = p->arena;
    if (!chunk || chunk->size - chunk->used < size) {
        size_t capacity = size > ARENA_CHUNK_SIZE ? size : ARENA_CHUNK_SIZE;
        chunk = calloc(1, sizeof(*chunk) + capacity);
        if (!chunk) return NULL;
        chunk->size = capacity;
        chunk->next = p->arena;
        p->arena = chunk;
    }
    void *ptr = chunk->data + chunk->used;
    chunk->used += size;
    return ptr;
}

static void *arena_array(json_protocol_t *p, size_t count, size_t size)
{
    return arena_alloc(p, count ? count * size : 1);
}

static const char *arena_strdup(json_protocol_t *p, const char *s)
{
    if (!s) return NULL;
    char *copy = arena_alloc(p, strlen(s) + 1);
    if (copy) strcpy(copy, s);
    return copy;
}

static const cJSON *member(const cJSON *object, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static bool get_int(const cJSON *item, int64_t min, int64_t max, int64_t *out)
{
    if (!cJSON_IsNumber(item)) return false;
    double v = item->valuedouble;
    if (v != floor(v) || v < (double)min || v > (double)max) return false;
    *out = (int64_t)v;
    return true;
}

static esp_err_t opt_int(const cJSON *object, const char *name, int64_t fallback, int64_t min, int64_t max, int64_t *out)
{
    const cJSON *item = member(object, name);
    if (!item) {
        *out = fallback;
        return ESP_OK;
    }
    return get_int(item, min, max, out) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t req_int(const cJSON *object, const char *name, int64_t min, int64_t max, int64_t *out)
{
    return get_int(member(object, name), min, max, out) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t list_size(const cJSON *item, bool required, size_t *count)
{
    *count = 0;
    if (!item) return required ? ESP_ERR_INVALID_ARG : ESP_OK;
    if (!cJSON_IsArray(item)) return ESP_ERR_INVALID_ARG;
    int size = cJSON_GetArraySize(item);
    if (size < 0 || size > LIST_MAX) return ESP_ERR_INVALID_SIZE;
    *count = (size_t)size;
    return ESP_OK;
}

static esp_err_t parse_field_value(ir_field_id_t id, const cJSON *item, int32_t *value)
{
    const ir_field_info_t *info = ir_field_info(id);
    int64_t v;
    switch (info->type) {
    case IR_FIELD_TYPE_BOOL:
        if (!cJSON_IsBool(item)) return ESP_ERR_INVALID_ARG;
        *value = cJSON_IsTrue(item);
        return ESP_OK;
    case IR_FIELD_TYPE_ENUM:
        return cJSON_IsString(item) && ir_field_enum_value(id, item->valuestring, value) == ESP_OK ? ESP_OK : ESP_ERR_INVALID_ARG;
    case IR_FIELD_TYPE_DECI: {
        if (!cJSON_IsNumber(item)) return ESP_ERR_INVALID_ARG;
        double scaled = item->valuedouble * DECI_SCALE;
        double rounded = round(scaled);
        if (fabs(scaled - rounded) > DECI_EPSILON || rounded < INT32_MIN || rounded > INT32_MAX) return ESP_ERR_INVALID_ARG;
        *value = (int32_t)rounded;
        return ESP_OK;
    }
    default:
        if (!get_int(item, INT32_MIN, INT32_MAX, &v)) return ESP_ERR_INVALID_ARG;
        *value = (int32_t)v;
        return ESP_OK;
    }
}

static esp_err_t parse_values(const cJSON *object, ir_values_t *values)
{
    memset(values, 0, sizeof(*values));
    if (!object) return ESP_OK;
    if (!cJSON_IsObject(object)) return ESP_ERR_INVALID_ARG;
    const cJSON *item;
    cJSON_ArrayForEach(item, object) {
        ir_field_id_t id;
        int32_t value;
        if (ir_field_find(item->string, &id) != ESP_OK) return ESP_ERR_INVALID_ARG;
        TRY(parse_field_value(id, item, &value));
        ir_values_set(values, id, value);
    }
    return ESP_OK;
}

static esp_err_t parse_limit(ir_field_id_t id, const cJSON *item, ir_limit_t *limit)
{
    ir_field_type_t type = ir_field_info(id)->type;
    memset(limit, 0, sizeof(*limit));
    if (type == IR_FIELD_TYPE_INT || type == IR_FIELD_TYPE_DECI) {
        const cJSON *step = member(item, "step");
        limit->step = type == IR_FIELD_TYPE_DECI ? DECI_SCALE : 1;
        if (!cJSON_IsObject(item)) return ESP_ERR_INVALID_ARG;
        TRY(parse_field_value(id, member(item, "min"), &limit->min));
        TRY(parse_field_value(id, member(item, "max"), &limit->max));
        if (step) TRY(parse_field_value(id, step, &limit->step));
        return limit->min <= limit->max && limit->step > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsArray(item)) return ESP_ERR_INVALID_ARG;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item) {
        int32_t value;
        TRY(parse_field_value(id, entry, &value));
        if (value < 0 || value >= IR_FIELD_ENUM_MAX) return ESP_ERR_INVALID_ARG;
        limit->mask |= 1UL << value;
    }
    return limit->mask ? ESP_OK : ESP_ERR_INVALID_ARG;
}

/* allow_unused: null marks a field the mode does not send, left as an all-zero limit. */
static esp_err_t parse_limits(const cJSON *object, bool allow_unused, uint32_t *fields, ir_limit_t *limits)
{
    if (!object) return ESP_OK;
    if (!cJSON_IsObject(object)) return ESP_ERR_INVALID_ARG;
    const cJSON *item;
    cJSON_ArrayForEach(item, object) {
        ir_field_id_t id;
        if (ir_field_find(item->string, &id) != ESP_OK) return ESP_ERR_INVALID_ARG;
        if (allow_unused && cJSON_IsNull(item)) {
            memset(&limits[id], 0, sizeof(limits[id]));
        } else {
            TRY(parse_limit(id, item, &limits[id]));
        }
        *fields |= IR_FIELD_BIT(id);
    }
    return ESP_OK;
}

static esp_err_t parse_mode_limits(json_protocol_t *p, const cJSON *object)
{
    if (!object) return ESP_OK;
    if (!cJSON_IsObject(object)) return ESP_ERR_INVALID_ARG;
    const cJSON *item;
    cJSON_ArrayForEach(item, object) {
        int32_t mode;
        if (ir_field_enum_value(IR_FIELD_MODE, item->string, &mode) != ESP_OK || p->meta.mode_limits[mode]) return ESP_ERR_INVALID_ARG;
        ir_limit_t *limits = arena_array(p, IR_FIELD_COUNT, sizeof(ir_limit_t));
        if (!limits) return ESP_ERR_NO_MEM;
        p->meta.mode_limits[mode] = limits;
        TRY(parse_limits(item, true, &p->meta.mode_limit_fields[mode], limits));
    }
    return ESP_OK;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static esp_err_t parse_hex(json_protocol_t *p, const cJSON *item, uint8_t length, const uint8_t **out)
{
    *out = NULL;
    if (!item) return ESP_OK;
    if (!cJSON_IsString(item) || strlen(item->valuestring) != 2U * length) return ESP_ERR_INVALID_ARG;
    uint8_t *bytes = arena_array(p, length, 1);
    if (!bytes) return ESP_ERR_NO_MEM;
    for (uint8_t i = 0; i < length; ++i) {
        int high = hex_digit(item->valuestring[2 * i]);
        int low = hex_digit(item->valuestring[2 * i + 1]);
        if (high < 0 || low < 0) return ESP_ERR_INVALID_ARG;
        bytes[i] = (uint8_t)(high << 4 | low);
    }
    *out = bytes;
    return ESP_OK;
}

/* "M9000" mark, "S4500" space, "B32" bits, "P108000" period. */
static esp_err_t parse_step(const cJSON *item, ir_step_t *step)
{
    if (!cJSON_IsString(item) || !item->valuestring[0] || !item->valuestring[1]) return ESP_ERR_INVALID_ARG;
    char *end;
    unsigned long value = strtoul(item->valuestring + 1, &end, 10);
    if (*end || !value || value > IR_FRAME_MAX_PULSE_US) return ESP_ERR_INVALID_ARG;
    memset(step, 0, sizeof(*step));
    switch (item->valuestring[0]) {
    case 'M': step->kind = IR_STEP_MARK; break;
    case 'S': step->kind = IR_STEP_SPACE; break;
    case 'P': step->kind = IR_STEP_PERIOD; break;
    case 'B':
        if (value > IR_BITS_MAX) return ESP_ERR_INVALID_ARG;
        step->kind = IR_STEP_BITS;
        step->count = (uint16_t)value;
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    step->value_us = (uint32_t)value;
    return ESP_OK;
}

static esp_err_t parse_layout(json_protocol_t *p, const cJSON *object, const ir_layout_t **out)
{
    if (!cJSON_IsObject(object)) return ESP_ERR_INVALID_ARG;
    ir_layout_t *layout = arena_alloc(p, sizeof(*layout));
    if (!layout) return ESP_ERR_NO_MEM;
    const cJSON *coding = member(object, "coding");
    if (coding && (!cJSON_IsString(coding) || (strcmp(coding->valuestring, "pair") && strcmp(coding->valuestring, "biphase")))) return ESP_ERR_INVALID_ARG;
    layout->coding = coding && !strcmp(coding->valuestring, "biphase") ? IR_CODING_BIPHASE : IR_CODING_PAIR;
    int64_t v;
    TRY(opt_int(object, "half_us", 0, 0, UINT16_MAX, &v));
    layout->half_us = (uint16_t)v;
    TRY(opt_int(object, "one_level", 1, 0, 1, &v));
    layout->one_level = (uint8_t)v;
    const cJSON *symbols = member(object, "symbols");
    size_t count;
    TRY(list_size(symbols, false, &count));
    if (count && count != 2 && count != IR_LAYOUT_SYMBOLS_MAX) return ESP_ERR_INVALID_ARG;
    layout->symbol_bits = count == IR_LAYOUT_SYMBOLS_MAX ? IR_LAYOUT_SYMBOL_BITS_MAX : 1;
    for (size_t i = 0; i < count; ++i) {
        const cJSON *symbol = cJSON_GetArrayItem(symbols, (int)i);
        int64_t mark, space;
        if (cJSON_GetArraySize(symbol) != 2 || !get_int(cJSON_GetArrayItem(symbol, 0), 1, UINT16_MAX, &mark) || !get_int(cJSON_GetArrayItem(symbol, 1), 1, UINT16_MAX, &space)) return ESP_ERR_INVALID_ARG;
        layout->symbols[i] = (ir_symbol_t){(uint16_t)mark, (uint16_t)space};
    }
    const cJSON *steps = member(object, "steps");
    TRY(list_size(steps, true, &count));
    ir_step_t *step_array = arena_array(p, count, sizeof(*step_array));
    if (!step_array) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < count; ++i) TRY(parse_step(cJSON_GetArrayItem(steps, (int)i), &step_array[i]));
    layout->steps = step_array;
    layout->step_count = (uint16_t)count;
    *out = layout;
    return ESP_OK;
}

static esp_err_t parse_bitfield(json_protocol_t *p, const cJSON *object, ir_bitfield_t *f)
{
    const cJSON *name = member(object, "field");
    ir_field_id_t id;
    if (!cJSON_IsString(name) || ir_field_find(name->valuestring, &id) != ESP_OK) return ESP_ERR_INVALID_ARG;
    f->id = (uint8_t)id;
    int64_t v;
    TRY(opt_int(object, "base", 0, INT32_MIN, INT32_MAX, &v));
    f->base = (int32_t)v;
    TRY(opt_int(object, "step", 0, 0, INT32_MAX, &v));
    f->step = (int32_t)v;
    const cJSON *segments = member(object, "segments");
    size_t count;
    TRY(list_size(segments, true, &count));
    ir_bit_segment_t *segment_array = arena_array(p, count, sizeof(*segment_array));
    if (!segment_array) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < count; ++i) {
        const cJSON *s = cJSON_GetArrayItem(segments, (int)i);
        int64_t byte, shift, width;
        if (cJSON_GetArraySize(s) != 3 || !get_int(cJSON_GetArrayItem(s, 0), 0, IR_PAYLOAD_MAX - 1, &byte) ||
            !get_int(cJSON_GetArrayItem(s, 1), 0, 7, &shift) || !get_int(cJSON_GetArrayItem(s, 2), 1, 8, &width)) return ESP_ERR_INVALID_ARG;
        segment_array[i] = (ir_bit_segment_t){(uint8_t)byte, (uint8_t)shift, (uint8_t)width};
    }
    f->segments = segment_array;
    f->segment_count = (uint8_t)count;
    const cJSON *raw = member(object, "raw");
    TRY(list_size(raw, false, &count));
    uint32_t *raw_array = count ? arena_array(p, count, sizeof(*raw_array)) : NULL;
    if (count && !raw_array) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < count; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(raw, (int)i);
        if (cJSON_IsNull(entry)) {
            raw_array[i] = IR_RAW_NONE;
        } else if (get_int(entry, 0, UINT32_MAX - 1, &v)) {
            raw_array[i] = (uint32_t)v;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    f->raw = raw_array;
    f->value_count = (uint8_t)count;
    return ESP_OK;
}

static esp_err_t parse_check(json_protocol_t *p, const cJSON *object, ir_check_t *c)
{
    static const char *const ops[] = {"copy", "sum", "xor"};
    const cJSON *op = member(object, "op");
    c->op = UINT8_MAX;
    for (uint8_t i = 0; cJSON_IsString(op) && i < sizeof(ops) / sizeof(ops[0]); ++i) {
        if (!strcmp(op->valuestring, ops[i])) c->op = i;
    }
    if (c->op == UINT8_MAX) return ESP_ERR_INVALID_ARG;
    int64_t v;
    TRY(req_int(object, "target", 0, UINT8_MAX, &v));
    c->target = (uint8_t)v;
    TRY(opt_int(object, "bias", 0, 0, UINT8_MAX, &v));
    c->bias = (uint8_t)v;
    static const struct { const char *name; uint8_t flag; } flags[] = {
        {"nibble", IR_CHECK_NIBBLE}, {"invert", IR_CHECK_INVERT}, {"byte_target", IR_CHECK_BYTE_TARGET},
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        const cJSON *flag = member(object, flags[i].name);
        if (flag && !cJSON_IsBool(flag)) return ESP_ERR_INVALID_ARG;
        if (cJSON_IsTrue(flag)) c->flags |= flags[i].flag;
    }
    const cJSON *units = member(object, "units");
    size_t count;
    TRY(list_size(units, false, &count));
    if (!units) {
        TRY(req_int(object, "first", 0, UINT8_MAX, &v));
        c->first = (uint8_t)v;
        TRY(req_int(object, "count", 1, UINT8_MAX, &v));
        c->count = (uint8_t)v;
        return ESP_OK;
    }
    uint8_t *unit_array = arena_array(p, count, 1);
    if (!unit_array) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < count; ++i) {
        if (!get_int(cJSON_GetArrayItem(units, (int)i), 0, UINT8_MAX, &v)) return ESP_ERR_INVALID_ARG;
        unit_array[i] = (uint8_t)v;
    }
    c->units = unit_array;
    c->count = (uint8_t)count;
    return ESP_OK;
}

static esp_err_t parse_message(json_protocol_t *p, const cJSON *object, ir_message_t *m)
{
    if (!cJSON_IsObject(object)) return ESP_ERR_INVALID_ARG;
    const cJSON *name = member(object, "name");
    m->name = arena_strdup(p, cJSON_IsString(name) ? name->valuestring : "frame");
    if (!m->name) return ESP_ERR_NO_MEM;
    TRY(parse_layout(p, member(object, "layout"), &m->layout));
    int64_t v;
    TRY(opt_int(object, "copies", 1, 1, UINT8_MAX, &v));
    m->copies = (uint8_t)v;
    TRY(opt_int(object, "length", 0, 0, IR_PAYLOAD_MAX, &v));
    m->length = (uint8_t)v;
    const cJSON *msb_first = member(object, "msb_first");
    if (msb_first && !cJSON_IsBool(msb_first)) return ESP_ERR_INVALID_ARG;
    m->msb_first = cJSON_IsTrue(msb_first);
    TRY(parse_hex(p, member(object, "template"), m->length, &m->template_bytes));
    TRY(parse_hex(p, member(object, "ignore"), m->length, &m->ignore));

    const cJSON *widths = member(object, "widths");
    size_t count;
    TRY(list_size(widths, false, &count));
    ir_bit_width_t *width_array = arena_array(p, count, sizeof(*width_array));
    if (!width_array) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < count; ++i) {
        const cJSON *w = cJSON_GetArrayItem(widths, (int)i);
        int64_t byte, bits;
        if (cJSON_GetArraySize(w) != 2 || !get_int(cJSON_GetArrayItem(w, 0), 0, IR_PAYLOAD_MAX - 1, &byte) || !get_int(cJSON_GetArrayItem(w, 1), 1, 8, &bits)) return ESP_ERR_INVALID_ARG;
        width_array[i] = (ir_bit_width_t){(uint8_t)byte, (uint8_t)bits};
    }
    m->widths = width_array;
    m->width_count = (uint8_t)count;

    const cJSON *fields = member(object, "fields");
    TRY(list_size(fields, false, &count));
    ir_bitfield_t *field_array = arena_array(p, count, sizeof(*field_array));
    if (!field_array) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < count; ++i) TRY(parse_bitfield(p, cJSON_GetArrayItem(fields, (int)i), &field_array[i]));
    m->fields = field_array;
    m->field_count = (uint8_t)count;

    const cJSON *checks = member(object, "checks");
    TRY(list_size(checks, false, &count));
    ir_check_t *check_array = arena_array(p, count, sizeof(*check_array));
    if (!check_array) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < count; ++i) TRY(parse_check(p, cJSON_GetArrayItem(checks, (int)i), &check_array[i]));
    m->checks = check_array;
    m->check_count = (uint8_t)count;

    const cJSON *implied = member(object, "implied");
    if (implied) {
        ir_values_t *values = arena_alloc(p, sizeof(*values));
        if (!values) return ESP_ERR_NO_MEM;
        TRY(parse_values(implied, values));
        m->implied = values;
    }
    return ESP_OK;
}

/* Only inline descriptions, so JSON protocols never carry IR_PROTOCOL_DECODE. */
static esp_err_t parse_codec(json_protocol_t *p, const cJSON *item)
{
    ir_protocol_t *protocol = &p->protocol;
    if (!cJSON_IsObject(item)) return ESP_ERR_INVALID_ARG;
    int64_t v;
    TRY(req_int(item, "carrier_hz", IR_FRAME_MIN_CARRIER_HZ, IR_FRAME_MAX_CARRIER_HZ, &v));
    protocol->carrier_hz = (uint32_t)v;
    const cJSON *messages = member(item, "messages");
    size_t count;
    TRY(list_size(messages, true, &count));
    ir_message_t *message_array = arena_array(p, count, sizeof(*message_array));
    if (!message_array) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < count; ++i) TRY(parse_message(p, cJSON_GetArrayItem(messages, (int)i), &message_array[i]));
    protocol->messages = message_array;
    protocol->message_count = (uint8_t)count;
    const cJSON *hold = member(item, "hold");
    if (hold) {
        ir_message_t *hold_message = arena_alloc(p, sizeof(*hold_message));
        if (!hold_message) return ESP_ERR_NO_MEM;
        TRY(parse_message(p, hold, hold_message));
        protocol->hold = hold_message;
    }
    return ir_protocol_validate(protocol);
}

static esp_err_t parse_info(json_protocol_t *p, const cJSON *root)
{
    const struct { const char *name; const char **target; } strings[] = {
        {"id", &p->protocol.id}, {"category", &p->meta.category}, {"brand", &p->meta.brand},
        {"model", &p->meta.model}, {"source", &p->meta.source}, {"maturity", &p->meta.maturity},
    };
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); ++i) {
        const cJSON *item = member(root, strings[i].name);
        if (!item) continue;
        if (!cJSON_IsString(item)) return ESP_ERR_INVALID_ARG;
        *strings[i].target = arena_strdup(p, item->valuestring);
        if (!*strings[i].target) return ESP_ERR_NO_MEM;
    }
    return ir_protocol_id_valid(p->protocol.id) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

/* Rejects unknown top-level keys, so a misspelt or retired section is not silently ignored. */
static esp_err_t check_root_keys(const cJSON *root)
{
    static const char *const known[] = {"id", "category", "brand", "model", "source", "maturity", "protocol", "limits", "mode_limits", "defaults"};
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        bool found = false;
        for (size_t i = 0; !found && i < sizeof(known) / sizeof(known[0]); ++i) found = !strcmp(item->string, known[i]);
        if (!found) return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t parse_root(json_protocol_t *p, const cJSON *root)
{
    if (!cJSON_IsObject(root)) return ESP_ERR_INVALID_ARG;
    TRY(check_root_keys(root));
    TRY(parse_info(p, root));
    TRY(parse_codec(p, member(root, "protocol")));
    TRY(parse_values(member(root, "defaults"), &p->meta.defaults));
    TRY(parse_limits(member(root, "limits"), false, &p->meta.limit_fields, p->meta.limits));
    TRY(parse_mode_limits(p, member(root, "mode_limits")));
    p->protocol.meta = &p->meta;
    if (p->meta.defaults.present & ~ir_protocol_fields(&p->protocol)) return ESP_ERR_INVALID_ARG;
    return ir_protocol_check(&p->protocol, &p->meta.defaults);
}

esp_err_t ir_protocol_parse_object(const cJSON *root, ir_protocol_t **ret_protocol)
{
    if (!root || !ret_protocol) return ESP_ERR_INVALID_ARG;
    *ret_protocol = NULL;
    json_protocol_t *p = calloc(1, sizeof(*p));
    if (!p) return ESP_ERR_NO_MEM;
    esp_err_t err = parse_root(p, root);
    if (err != ESP_OK) {
        ir_protocol_free(&p->protocol);
        return err;
    }
    *ret_protocol = &p->protocol;
    return ESP_OK;
}

esp_err_t ir_protocol_parse(const char *json, size_t length, ir_protocol_t **ret_protocol)
{
    if (!json || !ret_protocol) return ESP_ERR_INVALID_ARG;
    *ret_protocol = NULL;
    cJSON *root = cJSON_ParseWithLength(json, length);
    if (!root) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ir_protocol_parse_object(root, ret_protocol);
    cJSON_Delete(root);
    return err;
}

esp_err_t ir_protocol_load(const char *path, ir_protocol_t **ret_protocol)
{
    if (!path || !ret_protocol) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    long size = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    char *buffer = size > 0 && size <= IR_PROTOCOL_FILE_MAX ? malloc((size_t)size) : NULL;
    esp_err_t err = buffer ? ESP_OK : (size > 0 && size <= IR_PROTOCOL_FILE_MAX ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_SIZE);
    if (err == ESP_OK && (fseek(file, 0, SEEK_SET) != 0 || fread(buffer, 1, (size_t)size, file) != (size_t)size)) err = ESP_FAIL;
    fclose(file);
    if (err == ESP_OK) err = ir_protocol_parse(buffer, (size_t)size, ret_protocol);
    free(buffer);
    if (err != ESP_OK) ESP_LOGW(TAG, "load %s failed: %s", path, esp_err_to_name(err));
    return err;
}

void ir_protocol_free(ir_protocol_t *protocol)
{
    json_protocol_t *p = (json_protocol_t *)protocol;
    if (!p) return;
    while (p->arena) {
        arena_chunk_t *next = p->arena->next;
        free(p->arena);
        p->arena = next;
    }
    free(p);
}
