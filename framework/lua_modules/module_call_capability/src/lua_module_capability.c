/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_capability.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cap_lua.h"
#include "claw_cap.h"
#include "esp_err.h"
#include "lauxlib.h"

#define LUA_MODULE_CAPABILITY_NAME "capability"
#define LUA_MODULE_CAPABILITY_DEFAULT_OUTPUT_SIZE (64 * 1024)
#define LUA_MODULE_CAPABILITY_MAX_OUTPUT_SIZE     (256 * 1024)

static const char *lua_module_capability_get_string_field(lua_State *L,
                                                          int index,
                                                          const char *field_name)
{
    const char *value = NULL;

    index = lua_absindex(L, index);
    lua_getfield(L, index, field_name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return NULL;
    }
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "field '%s' must be a string", field_name);
    }

    value = lua_tostring(L, -1);
    lua_pop(L, 1);
    return value;
}

static const char *lua_module_capability_get_args_string_field(lua_State *L,
                                                               const char *field_name)
{
    const char *value = NULL;

    lua_getglobal(L, "args");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return NULL;
    }

    lua_getfield(L, -1, field_name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 2);
        return NULL;
    }
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 2);
        luaL_error(L, "global args.%s must be a string", field_name);
    }

    value = lua_tostring(L, -1);
    lua_pop(L, 2);
    return value;
}

static size_t lua_module_capability_get_size_field(lua_State *L,
                                                   int index,
                                                   const char *field_name,
                                                   size_t default_value,
                                                   size_t min_value,
                                                   size_t max_value)
{
    lua_Number value;
    size_t size_value;

    if (lua_isnoneornil(L, index)) {
        return default_value;
    }

    index = lua_absindex(L, index);
    lua_getfield(L, index, field_name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return default_value;
    }
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "field '%s' must be a number", field_name);
    }

    value = lua_tonumber(L, -1);
    lua_pop(L, 1);

    if (value < (lua_Number)min_value) {
        return min_value;
    }
    if (value > (lua_Number)max_value) {
        return max_value;
    }

    size_value = (size_t)value;
    return size_value > 0 ? size_value : default_value;
}

static cJSON *lua_module_capability_json_from_value(lua_State *L, int index);

static bool lua_module_capability_table_is_array(lua_State *L, int index)
{
    lua_Integer expected = 1;
    bool has_entries = false;

    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) != expected) {
            lua_pop(L, 2);
            return false;
        }
        has_entries = true;
        expected++;
        lua_pop(L, 1);
    }

    return has_entries;
}

static cJSON *lua_module_capability_json_from_table(lua_State *L, int index)
{
    cJSON *json = NULL;

    index = lua_absindex(L, index);
    if (lua_module_capability_table_is_array(L, index)) {
        json = cJSON_CreateArray();
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            cJSON *child = lua_module_capability_json_from_value(L, -1);

            lua_pop(L, 1);
            if (!child || !cJSON_AddItemToArray(json, child)) {
                cJSON_Delete(child);
                cJSON_Delete(json);
                return NULL;
            }
        }
        return json;
    }

    json = cJSON_CreateObject();
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        const char *key = NULL;
        char key_buf[32];
        cJSON *child = NULL;

        if (lua_type(L, -2) == LUA_TSTRING) {
            key = lua_tostring(L, -2);
        } else if (lua_isinteger(L, -2)) {
            snprintf(key_buf, sizeof(key_buf), "%lld", (long long)lua_tointeger(L, -2));
            key = key_buf;
        } else {
            lua_pop(L, 2);
            cJSON_Delete(json);
            return NULL;
        }

        child = lua_module_capability_json_from_value(L, -1);
        lua_pop(L, 1);
        if (!child || !cJSON_AddItemToObject(json, key, child)) {
            cJSON_Delete(child);
            cJSON_Delete(json);
            return NULL;
        }
    }

    return json;
}

static cJSON *lua_module_capability_json_from_value(lua_State *L, int index)
{
    index = lua_absindex(L, index);

    switch (lua_type(L, index)) {
    case LUA_TNIL:
        return cJSON_CreateNull();
    case LUA_TBOOLEAN:
        return cJSON_CreateBool(lua_toboolean(L, index));
    case LUA_TNUMBER:
        return cJSON_CreateNumber(lua_tonumber(L, index));
    case LUA_TSTRING:
        return cJSON_CreateString(lua_tostring(L, index));
    case LUA_TTABLE:
        return lua_module_capability_json_from_table(L, index);
    default:
        return NULL;
    }
}

static char *lua_module_capability_build_payload_json(lua_State *L, int index)
{
    cJSON *json = NULL;
    char *payload_json = NULL;

    if (lua_isnoneornil(L, index)) {
        payload_json = strdup("{}");
        if (!payload_json) {
            luaL_error(L, "out of memory");
        }
        return payload_json;
    }

    if (lua_type(L, index) == LUA_TSTRING) {
        cJSON *parsed = cJSON_Parse(lua_tostring(L, index));

        if (!parsed) {
            luaL_error(L, "payload string must be valid JSON");
        }
        cJSON_Delete(parsed);

        payload_json = strdup(lua_tostring(L, index));
        if (!payload_json) {
            luaL_error(L, "out of memory");
        }
        return payload_json;
    }

    if (!lua_istable(L, index)) {
        luaL_error(L, "payload must be nil, a table, or a JSON string");
    }

    json = lua_module_capability_json_from_value(L, index);
    if (!json) {
        luaL_error(L, "failed to convert payload to JSON");
    }

    payload_json = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!payload_json) {
        luaL_error(L, "failed to serialize payload as JSON");
    }

    return payload_json;
}

static void lua_module_capability_fill_context(lua_State *L, int opts_index, claw_cap_call_context_t *ctx)
{
    const char *value = NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->caller = CLAW_CAP_CALLER_SYSTEM;

    if (!lua_isnoneornil(L, opts_index)) {
        if (!lua_istable(L, opts_index)) {
            luaL_error(L, "opts must be a table");
        }

        value = lua_module_capability_get_string_field(L, opts_index, "session_id");
        if (value && value[0]) {
            ctx->session_id = value;
        }

        value = lua_module_capability_get_string_field(L, opts_index, "channel");
        if (value && value[0]) {
            ctx->channel = value;
        }

        value = lua_module_capability_get_string_field(L, opts_index, "chat_id");
        if (value && value[0]) {
            ctx->chat_id = value;
        }

        value = lua_module_capability_get_string_field(L, opts_index, "source_cap");
        if (value && value[0]) {
            ctx->source_cap = value;
        }
    }

    if (!ctx->session_id) {
        value = lua_module_capability_get_args_string_field(L, "session_id");
        if (value && value[0]) {
            ctx->session_id = value;
        }
    }
    if (!ctx->channel) {
        value = lua_module_capability_get_args_string_field(L, "channel");
        if (value && value[0]) {
            ctx->channel = value;
        }
    }
    if (!ctx->chat_id) {
        value = lua_module_capability_get_args_string_field(L, "chat_id");
        if (value && value[0]) {
            ctx->chat_id = value;
        }
    }
    if (!ctx->source_cap) {
        value = lua_module_capability_get_args_string_field(L, "source_cap");
        if (value && value[0]) {
            ctx->source_cap = value;
        }
    }
}

static int lua_module_capability_call(lua_State *L)
{
    const char *cap_name = luaL_checkstring(L, 1);
    claw_cap_call_context_t ctx = {0};
    char *payload_json = NULL;
    char *output = NULL;
    size_t output_size;
    esp_err_t err;

    payload_json = lua_module_capability_build_payload_json(L, 2);
    lua_module_capability_fill_context(L, 3, &ctx);
    output_size = lua_module_capability_get_size_field(L,
                                                       3,
                                                       "max_output_bytes",
                                                       LUA_MODULE_CAPABILITY_DEFAULT_OUTPUT_SIZE,
                                                       1024,
                                                       LUA_MODULE_CAPABILITY_MAX_OUTPUT_SIZE);

    output = calloc(1, output_size);
    if (!output) {
        free(payload_json);
        return luaL_error(L, "out of memory");
    }

    err = claw_cap_call(cap_name, payload_json, &ctx, output, output_size);
    free(payload_json);

    if (err == ESP_OK) {
        lua_pushboolean(L, 1);
        lua_pushstring(L, output);
        lua_pushnil(L);
        free(output);
        return 3;
    }

    lua_pushboolean(L, 0);
    if (output[0]) {
        lua_pushstring(L, output);
    } else {
        lua_pushnil(L);
    }
    lua_pushstring(L, esp_err_to_name(err));
    free(output);
    return 3;
}

int luaopen_capability(lua_State *L)
{
    lua_newtable(L);

    lua_pushcfunction(L, lua_module_capability_call);
    lua_setfield(L, -2, "call");

    return 1;
}

esp_err_t lua_module_capability_register(void)
{
    return cap_lua_register_module(LUA_MODULE_CAPABILITY_NAME, luaopen_capability);
}
