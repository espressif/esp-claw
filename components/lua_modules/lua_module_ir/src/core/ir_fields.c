/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ir_fields.h"

#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SWING_POSITION_PREFIX "position_"

static const char *const s_modes[] = {"auto", "cool", "dry", "heat", "fan"};
static const char *const s_fans[] = {"auto", "min", "low", "medium", "high", "max"};
static const char *const s_swings[] = {"off", "auto"};
static const char *const s_keys[] = {
    "power", "mode", "temp_up", "temp_down", "fan", "swing_v", "swing_v_fix", "swing_h",
    "turbo", "quiet", "econo", "light", "sleep", "clean", "filter", "beep",
};

#define ENUM_FIELD(field_name, names) {field_name, IR_FIELD_TYPE_ENUM, names, ARRAY_SIZE(names)}

static const ir_field_info_t s_fields[IR_FIELD_COUNT] = {
    [IR_FIELD_ADDRESS] = {"address", IR_FIELD_TYPE_INT},
    [IR_FIELD_COMMAND] = {"command", IR_FIELD_TYPE_INT},
    [IR_FIELD_TOGGLE] = {"toggle", IR_FIELD_TYPE_BOOL},
    [IR_FIELD_KEY] = ENUM_FIELD("key", s_keys),
    [IR_FIELD_POWER] = {"power", IR_FIELD_TYPE_BOOL},
    [IR_FIELD_MODE] = ENUM_FIELD("mode", s_modes),
    [IR_FIELD_TEMPERATURE] = {"temperature", IR_FIELD_TYPE_DECI},
    [IR_FIELD_FAN] = ENUM_FIELD("fan", s_fans),
    [IR_FIELD_SWING_V] = ENUM_FIELD("swing_v", s_swings),
    [IR_FIELD_SWING_H] = ENUM_FIELD("swing_h", s_swings),
    [IR_FIELD_TURBO] = {"turbo", IR_FIELD_TYPE_BOOL},
    [IR_FIELD_QUIET] = {"quiet", IR_FIELD_TYPE_BOOL},
    [IR_FIELD_ECONO] = {"econo", IR_FIELD_TYPE_BOOL},
    [IR_FIELD_LIGHT] = {"light", IR_FIELD_TYPE_BOOL},
    [IR_FIELD_SLEEP] = {"sleep", IR_FIELD_TYPE_BOOL},
    [IR_FIELD_CLEAN] = {"clean", IR_FIELD_TYPE_BOOL},
    [IR_FIELD_FILTER] = {"filter", IR_FIELD_TYPE_BOOL},
    [IR_FIELD_BEEP] = {"beep", IR_FIELD_TYPE_BOOL},
};

static bool is_swing(ir_field_id_t id)
{
    return id == IR_FIELD_SWING_V || id == IR_FIELD_SWING_H;
}

const ir_field_info_t *ir_field_info(ir_field_id_t id)
{
    return (unsigned)id < IR_FIELD_COUNT ? &s_fields[id] : NULL;
}

esp_err_t ir_field_find(const char *name, ir_field_id_t *id)
{
    for (unsigned i = 0; name && id && i < IR_FIELD_COUNT; ++i) {
        if (!strcmp(name, s_fields[i].name)) {
            *id = (ir_field_id_t)i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ir_field_enum_value(ir_field_id_t id, const char *name, int32_t *value)
{
    const ir_field_info_t *info = ir_field_info(id);
    if (!info || info->type != IR_FIELD_TYPE_ENUM || !name || !value) return ESP_ERR_INVALID_ARG;
    for (uint8_t i = 0; i < info->enum_count; ++i) {
        if (!strcmp(name, info->enum_names[i])) {
            *value = i;
            return ESP_OK;
        }
    }
    size_t prefix = strlen(SWING_POSITION_PREFIX);
    if (!is_swing(id) || strncmp(name, SWING_POSITION_PREFIX, prefix) || !name[prefix]) return ESP_ERR_NOT_FOUND;
    int32_t position = 0;
    for (const char *p = name + prefix; *p; ++p) {
        if (*p < '0' || *p > '9' || position > IR_FIELD_ENUM_MAX) return ESP_ERR_NOT_FOUND;
        position = position * 10 + (*p - '0');
    }
    if (position < 1 || position + IR_SWING_AUTO >= IR_FIELD_ENUM_MAX) return ESP_ERR_NOT_FOUND;
    *value = IR_SWING_AUTO + position;
    return ESP_OK;
}

esp_err_t ir_field_enum_name(ir_field_id_t id, int32_t value, char *buffer, size_t size)
{
    const ir_field_info_t *info = ir_field_info(id);
    if (!info || info->type != IR_FIELD_TYPE_ENUM || !buffer || !size || value < 0) return ESP_ERR_INVALID_ARG;
    int written;
    if (value < info->enum_count) {
        written = snprintf(buffer, size, "%s", info->enum_names[value]);
    } else if (is_swing(id) && value < IR_FIELD_ENUM_MAX) {
        written = snprintf(buffer, size, SWING_POSITION_PREFIX "%d", (int)(value - IR_SWING_AUTO));
    } else {
        return ESP_ERR_NOT_FOUND;
    }
    return written > 0 && (size_t)written < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

void ir_values_merge(ir_values_t *target, const ir_values_t *source)
{
    for (unsigned i = 0; i < IR_FIELD_COUNT; ++i) {
        if (source->present & IR_FIELD_BIT(i)) ir_values_set(target, (ir_field_id_t)i, source->value[i]);
    }
}

bool ir_values_contains(const ir_values_t *values, const ir_values_t *subset)
{
    if ((values->present & subset->present) != subset->present) return false;
    for (unsigned i = 0; i < IR_FIELD_COUNT; ++i) {
        if ((subset->present & IR_FIELD_BIT(i)) && values->value[i] != subset->value[i]) return false;
    }
    return true;
}
