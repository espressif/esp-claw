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

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IR_FIELD_ADDRESS,
    IR_FIELD_COMMAND,
    IR_FIELD_TOGGLE,
    IR_FIELD_KEY,               /* AC function code: which button produced the frame */
    IR_FIELD_POWER,
    IR_FIELD_MODE,
    IR_FIELD_TEMPERATURE,       /* 0.1 degrees Celsius */
    IR_FIELD_FAN,
    IR_FIELD_SWING_V,
    IR_FIELD_SWING_H,
    IR_FIELD_TURBO,
    IR_FIELD_QUIET,
    IR_FIELD_ECONO,
    IR_FIELD_LIGHT,
    IR_FIELD_SLEEP,
    IR_FIELD_CLEAN,
    IR_FIELD_FILTER,
    IR_FIELD_BEEP,
    IR_FIELD_COUNT,
} ir_field_id_t;

typedef enum {
    IR_FIELD_TYPE_INT,
    IR_FIELD_TYPE_BOOL,
    IR_FIELD_TYPE_ENUM,
    IR_FIELD_TYPE_DECI,         /* fixed point, one decimal digit */
} ir_field_type_t;

enum { IR_MODE_AUTO, IR_MODE_COOL, IR_MODE_DRY, IR_MODE_HEAT, IR_MODE_FAN, IR_MODE_COUNT };
enum { IR_FAN_AUTO, IR_FAN_MIN, IR_FAN_LOW, IR_FAN_MEDIUM, IR_FAN_HIGH, IR_FAN_MAX };
/* Swing values above IR_SWING_AUTO are position_1, position_2, ... */
enum { IR_SWING_OFF, IR_SWING_AUTO, IR_SWING_POSITION_1 };
enum {
    IR_KEY_POWER, IR_KEY_MODE, IR_KEY_TEMP_UP, IR_KEY_TEMP_DOWN, IR_KEY_FAN, IR_KEY_SWING_V, IR_KEY_SWING_V_FIX, IR_KEY_SWING_H,
    IR_KEY_TURBO, IR_KEY_QUIET, IR_KEY_ECONO, IR_KEY_LIGHT, IR_KEY_SLEEP, IR_KEY_CLEAN, IR_KEY_FILTER, IR_KEY_BEEP,
};

#define IR_FIELD_ENUM_MAX 32
#define IR_FIELD_BIT(id) (1UL << (id))
/* Fields that describe appliance state rather than identify a code or a key. */
#define IR_FIELD_STATE_MASK (~(IR_FIELD_BIT(IR_FIELD_ADDRESS) | IR_FIELD_BIT(IR_FIELD_COMMAND) | IR_FIELD_BIT(IR_FIELD_TOGGLE) | IR_FIELD_BIT(IR_FIELD_KEY)) & (IR_FIELD_BIT(IR_FIELD_COUNT) - 1U))

typedef struct {
    uint32_t present;           /* IR_FIELD_BIT(id) for each value set */
    int32_t value[IR_FIELD_COUNT];
} ir_values_t;

typedef struct {
    const char *name;           /* Lua and JSON key */
    ir_field_type_t type;
    const char *const *enum_names;
    uint8_t enum_count;
} ir_field_info_t;

const ir_field_info_t *ir_field_info(ir_field_id_t id);
esp_err_t ir_field_find(const char *name, ir_field_id_t *id);
esp_err_t ir_field_enum_value(ir_field_id_t id, const char *name, int32_t *value);
esp_err_t ir_field_enum_name(ir_field_id_t id, int32_t value, char *buffer, size_t size);

static inline bool ir_values_has(const ir_values_t *values, ir_field_id_t id)
{
    return (values->present & IR_FIELD_BIT(id)) != 0;
}

static inline void ir_values_set(ir_values_t *values, ir_field_id_t id, int32_t value)
{
    values->present |= IR_FIELD_BIT(id);
    values->value[id] = value;
}

/* Copies every value present in source over target. */
void ir_values_merge(ir_values_t *target, const ir_values_t *source);
/* True when every value in subset is present in values with the same value. */
bool ir_values_contains(const ir_values_t *values, const ir_values_t *subset);

#ifdef __cplusplus
}
#endif
