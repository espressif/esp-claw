/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_json.h"

#include <stdbool.h>
#include <stdio.h>

#include "cJSON.h"
#include "cap_lua.h"
#include "lauxlib.h"

#define LUA_MODULE_JSON_NAME "json"
#define LUA_MODULE_JSON_MAX_DEPTH 64

typedef struct {
    bool is_array;
    lua_Integer max_index;
    lua_Integer count;
} lua_module_json_table_shape_t;

static cJSON *lua_module_json_from_value(lua_State *L, int index, int depth);

static void lua_module_json_push_value(lua_State *L, const cJSON *item)
{
    cJSON *child = NULL;
    int index = 1;

    if (!item || cJSON_IsNull(item)) {
        lua_pushnil(L);
        return;
    }
    if (cJSON_IsBool(item)) {
        lua_pushboolean(L, cJSON_IsTrue(item));
        return;
    }
    if (cJSON_IsNumber(item)) {
        lua_pushnumber(L, item->valuedouble);
        return;
    }
    if (cJSON_IsString(item)) {
        lua_pushstring(L, item->valuestring ? item->valuestring : "");
        return;
    }
    if (cJSON_IsArray(item)) {
        lua_newtable(L);
        cJSON_ArrayForEach(child, item) {
            lua_module_json_push_value(L, child);
            lua_rawseti(L, -2, index++);
        }
        return;
    }
    if (cJSON_IsObject(item)) {
        lua_newtable(L);
        cJSON_ArrayForEach(child, item) {
            lua_module_json_push_value(L, child);
            lua_setfield(L, -2, child->string);
        }
        return;
    }

    lua_pushnil(L);
}

static lua_module_json_table_shape_t lua_module_json_get_table_shape(lua_State *L, int index)
{
    lua_module_json_table_shape_t shape = {
        .is_array = false,
        .max_index = 0,
        .count = 0,
    };

    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (!lua_isinteger(L, -2)) {
            lua_pop(L, 2);
            return shape;
        }

        lua_Integer key = lua_tointeger(L, -2);
        if (key <= 0) {
            lua_pop(L, 2);
            return shape;
        }
        if (key > shape.max_index) {
            shape.max_index = key;
        }
        shape.count++;
        lua_pop(L, 1);
    }

    shape.is_array = shape.count > 0 && shape.count == shape.max_index;
    return shape;
}

static cJSON *lua_module_json_from_array_table(lua_State *L,
                                               int index,
                                               lua_Integer count,
                                               int depth)
{
    cJSON *json = cJSON_CreateArray();

    if (!json) {
        return NULL;
    }

    index = lua_absindex(L, index);
    for (lua_Integer i = 1; i <= count; i++) {
        cJSON *child = NULL;

        lua_rawgeti(L, index, i);
        child = lua_module_json_from_value(L, -1, depth + 1);
        lua_pop(L, 1);
        if (!child || !cJSON_AddItemToArray(json, child)) {
            cJSON_Delete(child);
            cJSON_Delete(json);
            return NULL;
        }
    }

    return json;
}

static cJSON *lua_module_json_from_object_table(lua_State *L, int index, int depth)
{
    cJSON *json = cJSON_CreateObject();

    if (!json) {
        return NULL;
    }

    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        cJSON *child = lua_module_json_from_value(L, -1, depth + 1);
        char key_buf[32];
        const char *key = NULL;

        if (!child) {
            lua_pop(L, 1);
            cJSON_Delete(json);
            return NULL;
        }

        if (lua_type(L, -2) == LUA_TSTRING) {
            key = lua_tostring(L, -2);
        } else if (lua_isinteger(L, -2)) {
            snprintf(key_buf, sizeof(key_buf), "%lld", (long long)lua_tointeger(L, -2));
            key = key_buf;
        } else {
            cJSON_Delete(child);
            lua_pop(L, 1);
            cJSON_Delete(json);
            luaL_error(L, "json.encode: table keys must be strings or integers");
            return NULL;
        }

        if (!cJSON_AddItemToObject(json, key, child)) {
            cJSON_Delete(child);
            lua_pop(L, 1);
            cJSON_Delete(json);
            return NULL;
        }
        lua_pop(L, 1);
    }

    return json;
}

static cJSON *lua_module_json_from_table(lua_State *L, int index, int depth)
{
    lua_module_json_table_shape_t shape = lua_module_json_get_table_shape(L, index);

    if (shape.is_array) {
        return lua_module_json_from_array_table(L, index, shape.count, depth);
    }

    return lua_module_json_from_object_table(L, index, depth);
}

static cJSON *lua_module_json_from_value(lua_State *L, int index, int depth)
{
    if (depth > LUA_MODULE_JSON_MAX_DEPTH) {
        luaL_error(L, "json.encode: nested value too deep");
        return NULL;
    }

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
        return lua_module_json_from_table(L, index, depth);
    default:
        luaL_error(L, "json.encode: unsupported Lua type '%s'", luaL_typename(L, index));
        return NULL;
    }
}

static int lua_module_json_encode(lua_State *L)
{
    cJSON *json = lua_module_json_from_value(L, 1, 0);
    char *payload = NULL;

    if (!json) {
        return luaL_error(L, "json.encode: out of memory");
    }

    payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!payload) {
        return luaL_error(L, "json.encode: out of memory");
    }

    lua_pushstring(L, payload);
    cJSON_free(payload);
    return 1;
}

static int lua_module_json_decode(lua_State *L)
{
    const char *payload = luaL_checkstring(L, 1);
    cJSON *json = cJSON_Parse(payload);

    if (!json) {
        const char *error = cJSON_GetErrorPtr();
        if (error && error[0]) {
            return luaL_error(L, "json.decode: invalid JSON near '%.32s'", error);
        }
        return luaL_error(L, "json.decode: invalid JSON");
    }

    lua_module_json_push_value(L, json);
    cJSON_Delete(json);
    return 1;
}

int luaopen_json(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"encode", lua_module_json_encode},
        {"decode", lua_module_json_decode},
        {NULL, NULL},
    };

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t lua_module_json_register(void)
{
    return cap_lua_register_module(LUA_MODULE_JSON_NAME, luaopen_json);
}
