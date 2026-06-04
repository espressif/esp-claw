/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_event_publisher.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_event_publisher.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "lauxlib.h"

#include "cap_lua.h"

static const char *LUA_MODULE_EVENT_PUBLISHER_NAME = "event_publisher";

static const char *lua_table_get_string_field(lua_State *L,
                                              int index,
                                              const char *field_name,
                                              bool required)
{
    const char *value = NULL;
    int type;

    lua_getfield(L, index, field_name);
    type = lua_type(L, -1);
    if (type == LUA_TNIL) {
        lua_pop(L, 1);
        if (required) {
            luaL_error(L, "missing required field '%s'", field_name);
        }
        return NULL;
    }

    if (type != LUA_TSTRING) {
        lua_pop(L, 1);
        luaL_error(L, "field '%s' must be a string", field_name);
    }

    value = lua_tostring(L, -1);
    lua_pop(L, 1);
    return value;
}

static bool lua_table_get_integer_field(lua_State *L,
                                        int index,
                                        const char *field_name,
                                        int64_t *out_value)
{
    int is_num = 0;

    lua_getfield(L, index, field_name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }

    *out_value = (int64_t)lua_tointegerx(L, -1, &is_num);
    lua_pop(L, 1);
    if (!is_num) {
        luaL_error(L, "field '%s' must be an integer", field_name);
    }
    return true;
}

static cJSON *lua_module_event_publisher_json_from_value(lua_State *L, int index);

static bool lua_module_event_publisher_table_is_array(lua_State *L, int index)
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

static cJSON *lua_module_event_publisher_json_from_table(lua_State *L, int index)
{
    cJSON *json = NULL;

    index = lua_absindex(L, index);
    if (lua_module_event_publisher_table_is_array(L, index)) {
        json = cJSON_CreateArray();
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            cJSON *child = lua_module_event_publisher_json_from_value(L, -1);

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
            snprintf(key_buf, sizeof(key_buf), "%" PRId64, (int64_t)lua_tointeger(L, -2));
            key = key_buf;
        } else {
            lua_pop(L, 2);
            cJSON_Delete(json);
            return NULL;
        }

        child = lua_module_event_publisher_json_from_value(L, -1);
        lua_pop(L, 1);
        if (!child || !cJSON_AddItemToObject(json, key, child)) {
            cJSON_Delete(child);
            cJSON_Delete(json);
            return NULL;
        }
    }

    return json;
}

static cJSON *lua_module_event_publisher_json_from_value(lua_State *L, int index)
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
        return lua_module_event_publisher_json_from_table(L, index);
    default:
        return NULL;
    }
}

static char *lua_table_get_payload_json_field(lua_State *L, int index, bool default_empty_object)
{
    char *payload_json = NULL;
    cJSON *json = NULL;

    lua_getfield(L, index, "payload_json");
    if (!lua_isnil(L, -1)) {
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            luaL_error(L, "field 'payload_json' must be a string");
        }
        payload_json = strdup(lua_tostring(L, -1));
        lua_pop(L, 1);
        if (!payload_json) {
            luaL_error(L, "out of memory");
        }
        return payload_json;
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "payload");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (!default_empty_object) {
            return NULL;
        }
        payload_json = strdup("{}");
        if (!payload_json) {
            luaL_error(L, "out of memory");
        }
        return payload_json;
    }

    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "field 'payload' must be a table");
    }

    json = lua_module_event_publisher_json_from_value(L, -1);
    lua_pop(L, 1);
    if (!json) {
        luaL_error(L, "failed to convert 'payload' to JSON");
    }

    payload_json = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!payload_json) {
        luaL_error(L, "failed to serialize 'payload' as JSON");
    }

    return payload_json;
}

static claw_session_policy_t lua_module_event_publisher_parse_session_policy(lua_State *L,
                                                                                   int index,
                                                                                   bool *has_value)
{
    const char *policy = NULL;

    *has_value = false;
    lua_getfield(L, index, "session_policy");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return CLAW_SESSION_POLICY_CHAT;
    }
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "field 'session_policy' must be a string");
    }

    *has_value = true;
    policy = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (strcmp(policy, "chat") == 0) {
        return CLAW_SESSION_POLICY_CHAT;
    }
    if (strcmp(policy, "trigger") == 0) {
        return CLAW_SESSION_POLICY_TRIGGER;
    }
    if (strcmp(policy, "global") == 0) {
        return CLAW_SESSION_POLICY_GLOBAL;
    }
    if (strcmp(policy, "ephemeral") == 0) {
        return CLAW_SESSION_POLICY_EPHEMERAL;
    }
    if (strcmp(policy, "nosave") == 0) {
        return CLAW_SESSION_POLICY_NOSAVE;
    }

    luaL_error(L, "invalid session_policy '%s'", policy);
    return CLAW_SESSION_POLICY_CHAT;
}

static void lua_module_event_publisher_copy_field(char *dst,
                                                  size_t dst_size,
                                                  const char *value)
{
    if (value) {
        strlcpy(dst, value, dst_size);
    }
}

static void lua_module_event_publisher_raise_publish_error(lua_State *L, esp_err_t err)
{
    luaL_error(L, "event publish failed: %s", esp_err_to_name(err));
}

static int lua_event_publisher_publish_message(lua_State *L)
{
    const char *source_cap = NULL;
    const char *channel = NULL;
    const char *chat_id = NULL;
    const char *text = NULL;
    const char *sender_id = NULL;
    const char *message_id = NULL;
    esp_err_t err;

    luaL_checktype(L, 1, LUA_TTABLE);
    source_cap = lua_table_get_string_field(L, 1, "source_cap", true);
    channel = lua_table_get_string_field(L, 1, "channel", true);
    chat_id = lua_table_get_string_field(L, 1, "chat_id", true);
    text = lua_table_get_string_field(L, 1, "text", true);
    sender_id = lua_table_get_string_field(L, 1, "sender_id", false);
    message_id = lua_table_get_string_field(L, 1, "message_id", false);

    err = claw_event_router_publish_message(source_cap,
                                            channel,
                                            chat_id,
                                            text,
                                            sender_id,
                                            message_id);
    if (err != ESP_OK) {
        lua_module_event_publisher_raise_publish_error(L, err);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_event_publisher_publish_trigger(lua_State *L)
{
    const char *source_cap = NULL;
    const char *event_type = NULL;
    const char *event_key = NULL;
    char *payload_json = NULL;
    esp_err_t err;

    luaL_checktype(L, 1, LUA_TTABLE);
    source_cap = lua_table_get_string_field(L, 1, "source_cap", true);
    event_type = lua_table_get_string_field(L, 1, "event_type", true);
    event_key = lua_table_get_string_field(L, 1, "event_key", true);
    payload_json = lua_table_get_payload_json_field(L, 1, true);

    err = claw_event_router_publish_trigger(source_cap,
                                            event_type,
                                            event_key,
                                            payload_json);
    free(payload_json);
    if (err != ESP_OK) {
        lua_module_event_publisher_raise_publish_error(L, err);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_event_publisher_publish(lua_State *L)
{
    claw_event_t event = {0};
    const char *event_id = NULL;
    const char *source_cap = NULL;
    const char *event_type = NULL;
    const char *source_channel = NULL;
    const char *target_channel = NULL;
    const char *source_endpoint = NULL;
    const char *target_endpoint = NULL;
    const char *chat_id = NULL;
    const char *sender_id = NULL;
    const char *message_id = NULL;
    const char *correlation_id = NULL;
    const char *content_type = NULL;
    const char *text = NULL;
    char *payload_json = NULL;
    int64_t timestamp_ms = 0;
    bool has_timestamp = false;
    bool has_policy = false;
    esp_err_t err;

    luaL_checktype(L, 1, LUA_TTABLE);

    source_cap = lua_table_get_string_field(L, 1, "source_cap", true);
    event_type = lua_table_get_string_field(L, 1, "event_type", true);
    event_id = lua_table_get_string_field(L, 1, "event_id", false);
    source_channel = lua_table_get_string_field(L, 1, "source_channel", false);
    target_channel = lua_table_get_string_field(L, 1, "target_channel", false);
    source_endpoint = lua_table_get_string_field(L, 1, "source_endpoint", false);
    target_endpoint = lua_table_get_string_field(L, 1, "target_endpoint", false);
    chat_id = lua_table_get_string_field(L, 1, "chat_id", false);
    sender_id = lua_table_get_string_field(L, 1, "sender_id", false);
    message_id = lua_table_get_string_field(L, 1, "message_id", false);
    correlation_id = lua_table_get_string_field(L, 1, "correlation_id", false);
    content_type = lua_table_get_string_field(L, 1, "content_type", false);
    text = lua_table_get_string_field(L, 1, "text", false);
    payload_json = lua_table_get_payload_json_field(L, 1, false);
    has_timestamp = lua_table_get_integer_field(L, 1, "timestamp_ms", &timestamp_ms);

    lua_module_event_publisher_copy_field(event.source_cap, sizeof(event.source_cap), source_cap);
    lua_module_event_publisher_copy_field(event.event_type, sizeof(event.event_type), event_type);
    lua_module_event_publisher_copy_field(event.source_channel, sizeof(event.source_channel), source_channel);
    lua_module_event_publisher_copy_field(event.target_channel, sizeof(event.target_channel), target_channel);
    lua_module_event_publisher_copy_field(event.source_endpoint, sizeof(event.source_endpoint), source_endpoint);
    lua_module_event_publisher_copy_field(event.target_endpoint, sizeof(event.target_endpoint), target_endpoint);
    lua_module_event_publisher_copy_field(event.chat_id, sizeof(event.chat_id), chat_id);
    lua_module_event_publisher_copy_field(event.sender_id, sizeof(event.sender_id), sender_id);
    lua_module_event_publisher_copy_field(event.message_id, sizeof(event.message_id), message_id);
    lua_module_event_publisher_copy_field(event.correlation_id, sizeof(event.correlation_id), correlation_id);
    lua_module_event_publisher_copy_field(event.content_type, sizeof(event.content_type), content_type);

    if (event_id) {
        lua_module_event_publisher_copy_field(event.event_id, sizeof(event.event_id), event_id);
    } else {
        snprintf(event.event_id, sizeof(event.event_id), "lua-%" PRId64, esp_timer_get_time() / 1000);
    }

    event.timestamp_ms = has_timestamp ? timestamp_ms : (esp_timer_get_time() / 1000);
    event.session_policy = lua_module_event_publisher_parse_session_policy(L, 1, &has_policy);
    if (!has_policy && strcmp(event_type, "trigger") == 0) {
        event.session_policy = CLAW_SESSION_POLICY_TRIGGER;
    }

    event.text = (char *)text;
    event.payload_json = payload_json;

    err = claw_event_router_publish(&event);
    free(payload_json);
    if (err != ESP_OK) {
        lua_module_event_publisher_raise_publish_error(L, err);
    }

    lua_pushboolean(L, 1);
    return 1;
}

int luaopen_event_publisher(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_event_publisher_publish);
    lua_setfield(L, -2, "publish");
    lua_pushcfunction(L, lua_event_publisher_publish_message);
    lua_setfield(L, -2, "publish_message");
    lua_pushcfunction(L, lua_event_publisher_publish_trigger);
    lua_setfield(L, -2, "publish_trigger");
    return 1;
}

esp_err_t lua_module_event_publisher_register(void)
{
    return cap_lua_register_module(LUA_MODULE_EVENT_PUBLISHER_NAME,
                                   luaopen_event_publisher);
}
