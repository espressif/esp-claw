/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_thread.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cap_lua.h"
#include "esp_err.h"
#include "lauxlib.h"

#define THREAD_JOB_OUTPUT_SIZE 4096
#define THREAD_JOB_JSON_MAX_DEPTH 64

typedef struct {
    bool is_array;
    lua_Integer max_index;
    lua_Integer count;
} thread_job_table_shape_t;

static cJSON *thread_job_json_from_value(lua_State *L, int index, int depth);

static thread_job_table_shape_t thread_job_get_table_shape(lua_State *L, int index)
{
    thread_job_table_shape_t shape = {
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

static cJSON *thread_job_json_from_array(lua_State *L,
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
        child = thread_job_json_from_value(L, -1, depth + 1);
        lua_pop(L, 1);
        if (!child || !cJSON_AddItemToArray(json, child)) {
            cJSON_Delete(child);
            cJSON_Delete(json);
            return NULL;
        }
    }

    return json;
}

static cJSON *thread_job_json_from_object(lua_State *L, int index, int depth)
{
    cJSON *json = cJSON_CreateObject();

    if (!json) {
        return NULL;
    }

    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        cJSON *child = thread_job_json_from_value(L, -1, depth + 1);
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
            luaL_error(L, "thread: table keys must be strings or integers");
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

static cJSON *thread_job_json_from_table(lua_State *L, int index, int depth)
{
    thread_job_table_shape_t shape = thread_job_get_table_shape(L, index);

    if (shape.is_array) {
        return thread_job_json_from_array(L, index, shape.count, depth);
    }

    return thread_job_json_from_object(L, index, depth);
}

static cJSON *thread_job_json_from_value(lua_State *L, int index, int depth)
{
    if (depth > THREAD_JOB_JSON_MAX_DEPTH) {
        luaL_error(L, "thread: nested value too deep");
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
        return thread_job_json_from_table(L, index, depth);
    default:
        luaL_error(L, "thread: unsupported Lua type '%s'", luaL_typename(L, index));
        return NULL;
    }
}

static char *thread_job_build_args_json(lua_State *L, int index)
{
    cJSON *json = NULL;
    char *payload = NULL;

    if (lua_isnoneornil(L, index)) {
        return NULL;
    }
    if (!lua_istable(L, index)) {
        luaL_error(L, "thread: args must be a table or nil");
        return NULL;
    }

    json = thread_job_json_from_table(L, index, 0);
    if (!json) {
        luaL_error(L, "thread: out of memory");
        return NULL;
    }
    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        luaL_error(L, "thread: args must encode to a JSON object");
        return NULL;
    }

    payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!payload) {
        luaL_error(L, "thread: out of memory");
        return NULL;
    }

    return payload;
}

static uint32_t thread_job_get_timeout_ms(lua_State *L,
                                                 int opts_idx,
                                                 uint32_t default_value)
{
    lua_Integer value = default_value;

    if (lua_isnoneornil(L, opts_idx)) {
        return default_value;
    }
    if (!lua_istable(L, opts_idx)) {
        luaL_error(L, "thread: opts must be a table or nil");
        return default_value;
    }

    opts_idx = lua_absindex(L, opts_idx);
    lua_getfield(L, opts_idx, "timeout_ms");
    if (!lua_isnil(L, -1)) {
        value = luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);

    if (value < 0 || (uint64_t)value > UINT32_MAX) {
        luaL_error(L, "thread: opts.timeout_ms must be 0..%u", UINT32_MAX);
    }
    return (uint32_t)value;
}

static bool thread_job_get_bool_field(lua_State *L,
                                             int opts_idx,
                                             const char *field_name,
                                             bool default_value)
{
    bool value = default_value;

    if (lua_isnoneornil(L, opts_idx)) {
        return default_value;
    }
    if (!lua_istable(L, opts_idx)) {
        luaL_error(L, "thread: opts must be a table or nil");
        return default_value;
    }

    opts_idx = lua_absindex(L, opts_idx);
    lua_getfield(L, opts_idx, field_name);
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1)) {
            lua_pop(L, 1);
            luaL_error(L, "thread: opts.%s must be a boolean", field_name);
            return default_value;
        }
        value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static const char *thread_job_get_string_field(lua_State *L,
                                                      int opts_idx,
                                                      const char *field_name,
                                                      char *buf,
                                                      size_t buf_size)
{
    const char *value = NULL;

    if (buf && buf_size > 0) {
        buf[0] = '\0';
    }
    if (lua_isnoneornil(L, opts_idx)) {
        return NULL;
    }
    if (!lua_istable(L, opts_idx)) {
        luaL_error(L, "thread: opts must be a table or nil");
        return NULL;
    }

    opts_idx = lua_absindex(L, opts_idx);
    lua_getfield(L, opts_idx, field_name);
    if (!lua_isnil(L, -1)) {
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            luaL_error(L, "thread: opts.%s must be a string", field_name);
            return NULL;
        }
        value = lua_tostring(L, -1);
        if (buf && buf_size > 0) {
            strlcpy(buf, value, buf_size);
            value = buf;
        }
    }
    lua_pop(L, 1);
    return value && value[0] ? value : NULL;
}

static int thread_job_push_result(lua_State *L, esp_err_t err, const char *output)
{
    if (err == ESP_OK) {
        lua_pushboolean(L, true);
        lua_pushstring(L, output ? output : "");
        return 2;
    }

    lua_pushnil(L);
    if (output && output[0]) {
        lua_pushstring(L, output);
    } else {
        lua_pushstring(L, esp_err_to_name(err));
    }
    return 2;
}

static int thread_job_run(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    /* Parse opts (which can raise -> longjmp) before building args_json so a bad
     * opts field cannot leak it. */
    uint32_t timeout_ms = thread_job_get_timeout_ms(L, 3, 0);
    char *args_json = thread_job_build_args_json(L, 2);
    char output[THREAD_JOB_OUTPUT_SIZE] = {0};
    esp_err_t err = cap_lua_run_script(path,
                                       args_json,
                                       timeout_ms,
                                       output,
                                       sizeof(output));

    cJSON_free(args_json);
    return thread_job_push_result(L, err, output);
}

static int thread_job_start(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    /* Parse opts (which can raise -> longjmp) before building args_json so a bad
     * opts field cannot leak it. */
    uint32_t timeout_ms = thread_job_get_timeout_ms(L, 3, 0);
    char name[64] = {0};
    char exclusive[64] = {0};
    const char *name_value = thread_job_get_string_field(L, 3, "name", name, sizeof(name));
    const char *exclusive_value = thread_job_get_string_field(L, 3, "exclusive", exclusive, sizeof(exclusive));
    bool replace = thread_job_get_bool_field(L, 3, "replace", false);
    char *args_json = thread_job_build_args_json(L, 2);
    char output[THREAD_JOB_OUTPUT_SIZE] = {0};
    esp_err_t err = cap_lua_run_script_async(path,
                                             args_json,
                                             timeout_ms,
                                             name_value,
                                             exclusive_value,
                                             replace,
                                             output,
                                             sizeof(output));

    cJSON_free(args_json);
    return thread_job_push_result(L, err, output);
}

static int thread_job_list(lua_State *L)
{
    const char *status = luaL_optstring(L, 1, NULL);
    char output[THREAD_JOB_OUTPUT_SIZE] = {0};
    esp_err_t err = cap_lua_list_jobs(status, output, sizeof(output));

    return thread_job_push_result(L, err, output);
}

static int thread_job_get(lua_State *L)
{
    const char *id_or_name = luaL_checkstring(L, 1);
    char output[THREAD_JOB_OUTPUT_SIZE] = {0};
    esp_err_t err = cap_lua_get_job(id_or_name, output, sizeof(output));

    return thread_job_push_result(L, err, output);
}

static int thread_job_stop(lua_State *L)
{
    const char *id_or_name = luaL_checkstring(L, 1);
    lua_Integer wait_value = luaL_optinteger(L, 2, 0);
    char output[THREAD_JOB_OUTPUT_SIZE] = {0};
    esp_err_t err;

    if (wait_value < 0 || (uint64_t)wait_value > UINT32_MAX) {
        return luaL_error(L, "thread.stop: wait_ms must be 0..%u", UINT32_MAX);
    }

    err = cap_lua_stop_job(id_or_name, (uint32_t)wait_value, output, sizeof(output));
    return thread_job_push_result(L, err, output);
}

void lua_module_thread_register_job_funcs(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"run", thread_job_run},
        {"start", thread_job_start},
        {"list", thread_job_list},
        {"get", thread_job_get},
        {"stop", thread_job_stop},
        {NULL, NULL},
    };

    luaL_checktype(L, -1, LUA_TTABLE);
    luaL_setfuncs(L, funcs, 0);
}
