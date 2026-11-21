/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_ir_common.h"

#include <math.h>
#include <string.h>

#include "lauxlib.h"

#define DECI_SCALE 10
/* Loose enough for LUA_32BITS, where lua_Number is a float. */
#define DECI_EPSILON 1e-3
#define ENUM_NAME_MAX 16

static bool listed(const char *key, const char *const *list, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(key, list[i])) return true;
    }
    return false;
}

static int32_t read_value(lua_State *L, int index, ir_field_id_t id, const char *context)
{
    const ir_field_info_t *info = ir_field_info(id);
    int32_t value = 0;
    if (info->type == IR_FIELD_TYPE_BOOL) {
        if (!lua_isboolean(L, index)) luaL_error(L, "%s: %s must be a boolean", context, info->name);
        value = lua_toboolean(L, index);
    } else if (info->type == IR_FIELD_TYPE_ENUM) {
        if (lua_type(L, index) != LUA_TSTRING || ir_field_enum_value(id, lua_tostring(L, index), &value) != ESP_OK) luaL_error(L, "%s: invalid %s", context, info->name);
    } else if (info->type == IR_FIELD_TYPE_DECI) {
        double scaled = lua_type(L, index) == LUA_TNUMBER ? lua_tonumber(L, index) * DECI_SCALE : NAN;
        double rounded = round(scaled);
        if (!(fabs(scaled - rounded) <= DECI_EPSILON) || rounded < INT32_MIN || rounded > INT32_MAX) luaL_error(L, "%s: %s must be a number with at most one decimal", context, info->name);
        value = (int32_t)rounded;
    } else {
        int64_t v = lua_isinteger(L, index) ? (int64_t)lua_tointeger(L, index) : (int64_t)INT32_MAX + 1;
        if (v < INT32_MIN || v > INT32_MAX) luaL_error(L, "%s: %s must be a 32-bit integer", context, info->name);
        value = (int32_t)v;
    }
    return value;
}

void lua_ir_values_read(lua_State *L, int index, const char *context, ir_values_t *values)
{
    index = lua_absindex(L, index);
    luaL_checktype(L, index, LUA_TTABLE);
    memset(values, 0, sizeof(*values));
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "%s: field names must be strings", context);
        const char *key = lua_tostring(L, -2);
        ir_field_id_t id;
        if (ir_field_find(key, &id) == ESP_OK) {
            ir_values_set(values, id, read_value(L, -1, id, context));
        } else {
            luaL_error(L, "%s: unknown field '%s'", context, key);
        }
        lua_pop(L, 1);
    }
}

void lua_ir_value_push(lua_State *L, ir_field_id_t id, int32_t value)
{
    const ir_field_info_t *info = ir_field_info(id);
    char name[ENUM_NAME_MAX];
    if (info->type == IR_FIELD_TYPE_BOOL) {
        lua_pushboolean(L, value);
    } else if (info->type == IR_FIELD_TYPE_ENUM && ir_field_enum_name(id, value, name, sizeof(name)) == ESP_OK) {
        lua_pushstring(L, name);
    } else if (info->type == IR_FIELD_TYPE_DECI && value % DECI_SCALE) {
        lua_pushnumber(L, (lua_Number)value / DECI_SCALE);
    } else {
        lua_pushinteger(L, info->type == IR_FIELD_TYPE_DECI ? value / DECI_SCALE : value);
    }
}

void lua_ir_values_write(lua_State *L, const ir_values_t *values)
{
    for (unsigned i = 0; i < IR_FIELD_COUNT; ++i) {
        if (!ir_values_has(values, (ir_field_id_t)i)) continue;
        lua_ir_value_push(L, (ir_field_id_t)i, values->value[i]);
        lua_setfield(L, -2, ir_field_info((ir_field_id_t)i)->name);
    }
}

void lua_ir_check_keys(lua_State *L, int index, const char *context, const char *const *allowed, size_t allowed_count)
{
    index = lua_absindex(L, index);
    luaL_checktype(L, index, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING || !listed(lua_tostring(L, -2), allowed, allowed_count)) {
            luaL_error(L, "%s: unknown field '%s'", context, lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "?");
        }
        lua_pop(L, 1);
    }
}

const char *lua_ir_reason(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_FOUND: return "no_match";
    case ESP_ERR_INVALID_CRC: return "checksum";
    case ESP_ERR_NOT_SUPPORTED: return "unsupported_value";
    default: return NULL;
    }
}

const char *lua_ir_error_text(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_ARG: return "missing or invalid field";
    case ESP_ERR_NOT_SUPPORTED: return "unsupported value";
    case ESP_ERR_NOT_FOUND: return "not found";
    case ESP_ERR_INVALID_SIZE: return "too large";
    case ESP_ERR_NO_MEM: return "out of memory";
    default: return esp_err_to_name(err);
    }
}
