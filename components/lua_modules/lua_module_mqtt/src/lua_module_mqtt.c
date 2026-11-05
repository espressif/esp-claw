/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_mqtt.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lauxlib.h"
#include "mqtt_client.h"

#define LUA_MQTT_CLIENT_MT              "mqtt.client"
#define LUA_MQTT_DEFAULT_QUEUE_LEN      16
#define LUA_MQTT_DEFAULT_MESSAGE_MAX    16384
#define LUA_MQTT_PROCESS_MAX            8
#define LUA_MQTT_FILE_MAX               65536

static const char *TAG = "lua_mqtt";
static const char s_state_key;

typedef enum {
    LUA_MQTT_EVENT_BEFORE_CONNECT,
    LUA_MQTT_EVENT_CONNECTED,
    LUA_MQTT_EVENT_DISCONNECTED,
    LUA_MQTT_EVENT_SUBSCRIBED,
    LUA_MQTT_EVENT_UNSUBSCRIBED,
    LUA_MQTT_EVENT_PUBLISHED,
    LUA_MQTT_EVENT_MESSAGE,
    LUA_MQTT_EVENT_ERROR,
    LUA_MQTT_EVENT_OVERFLOW,
    LUA_MQTT_EVENT_MAX,
} lua_mqtt_event_type_t;

typedef enum {
    LUA_MQTT_OVERFLOW_QUEUE,
    LUA_MQTT_OVERFLOW_MESSAGE_SIZE,
    LUA_MQTT_OVERFLOW_FRAGMENT,
    LUA_MQTT_OVERFLOW_MEMORY,
} lua_mqtt_overflow_t;

typedef struct lua_mqtt_event {
    lua_mqtt_event_type_t type;
    int msg_id;
    int qos;
    int granted_qos;
    int reason_code;
    int session_present;
    bool retain;
    bool dup;
    int error_type;
    int connect_return_code;
    int tls_error;
    int tls_stack_error;
    int cert_verify_flags;
    int socket_errno;
    lua_mqtt_overflow_t overflow;
    char *topic;
    size_t topic_len;
    char *payload;
    size_t payload_len;
    size_t received;
} lua_mqtt_event_t;

typedef struct {
    char *uri;
    char *client_id;
    char *username;
    char *password;
    char *will_topic;
    char *will_payload;
    size_t will_payload_len;
    char *ca;
    char *client_cert;
    char *client_key;
} lua_mqtt_owned_config_t;

typedef struct lua_mqtt_client lua_mqtt_client_t;

typedef struct {
    lua_mqtt_client_t *client;
} lua_mqtt_client_ud_t;

struct lua_mqtt_client {
    esp_mqtt_client_handle_t native;
    lua_State *L;
    QueueHandle_t queue;
    lua_mqtt_event_t *incoming;
    lua_mqtt_owned_config_t owned;
    lua_mqtt_client_ud_t *owner_ud;
    lua_mqtt_client_t *next;
    size_t max_message_size;
    int callback_refs[LUA_MQTT_EVENT_MAX];
    unsigned dispatch_depth;
    bool started;
    bool close_requested;
    bool closed;
    atomic_bool closing;
    atomic_uint events_dropped;
    atomic_uint messages_dropped;
    atomic_uint callback_errors;
    atomic_uint messages_received;
};

typedef struct {
    lua_mqtt_client_t *clients;
} lua_mqtt_state_t;

static const char *const s_event_names[LUA_MQTT_EVENT_MAX] = {
    "before_connect", "connected", "disconnected", "subscribed", "unsubscribed",
    "published", "message", "error", "overflow",
};

static void lua_mqtt_event_free(lua_mqtt_event_t *event)
{
    if (!event) return;
    free(event->topic);
    free(event->payload);
    free(event);
}

static void lua_mqtt_owned_config_free(lua_mqtt_owned_config_t *owned)
{
    if (!owned) return;
    free(owned->uri);
    free(owned->client_id);
    free(owned->username);
    free(owned->password);
    free(owned->will_topic);
    free(owned->will_payload);
    free(owned->ca);
    free(owned->client_cert);
    free(owned->client_key);
    memset(owned, 0, sizeof(*owned));
}

static lua_mqtt_state_t *lua_mqtt_state(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &s_state_key);
    lua_mqtt_state_t *state = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return state;
}

static void lua_mqtt_unlink(lua_State *L, lua_mqtt_client_t *client)
{
    lua_mqtt_state_t *state = lua_mqtt_state(L);
    if (!state) return;
    lua_mqtt_client_t **cursor = &state->clients;
    while (*cursor) {
        if (*cursor == client) {
            *cursor = client->next;
            client->next = NULL;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static char *lua_mqtt_dup_lstring(lua_State *L, int index, size_t *out_len)
{
    size_t len = 0;
    const char *value = luaL_checklstring(L, index, &len);
    char *copy = malloc(len + 1);
    if (!copy) luaL_error(L, "out of memory");
    memcpy(copy, value, len);
    copy[len] = '\0';
    if (out_len) *out_len = len;
    return copy;
}

static char *lua_mqtt_get_string_field(lua_State *L, int table, const char *name)
{
    char *value = NULL;
    table = lua_absindex(L, table);
    lua_getfield(L, table, name);
    if (!lua_isnil(L, -1)) {
        size_t len = 0;
        const char *source = luaL_checklstring(L, -1, &len);
        if (strlen(source) != len) luaL_error(L, "%s must not contain NUL bytes", name);
        value = lua_mqtt_dup_lstring(L, -1, NULL);
    }
    lua_pop(L, 1);
    return value;
}

static int lua_mqtt_get_integer_field(lua_State *L, int table, const char *name, int fallback)
{
    int value = fallback;
    table = lua_absindex(L, table);
    lua_getfield(L, table, name);
    if (!lua_isnil(L, -1)) value = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return value;
}

static bool lua_mqtt_get_boolean_field(lua_State *L, int table, const char *name, bool fallback)
{
    bool value = fallback;
    table = lua_absindex(L, table);
    lua_getfield(L, table, name);
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TBOOLEAN);
        value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static char *lua_mqtt_read_file(lua_State *L, const char *path)
{
    if (!path || path[0] != '/' || strstr(path, "..")) luaL_error(L, "certificate path must be an absolute safe path");
    FILE *file = fopen(path, "rb");
    if (!file) luaL_error(L, "failed to open certificate file: %s", path);
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        luaL_error(L, "failed to inspect certificate file: %s", path);
    }
    long size = ftell(file);
    if (size <= 0 || size > LUA_MQTT_FILE_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        luaL_error(L, "invalid certificate file size: %s", path);
    }
    char *data = malloc((size_t)size + 1);
    if (!data) {
        fclose(file);
        luaL_error(L, "out of memory");
    }
    size_t read_len = fread(data, 1, (size_t)size, file);
    fclose(file);
    if (read_len != (size_t)size) {
        free(data);
        luaL_error(L, "failed to read certificate file: %s", path);
    }
    data[size] = '\0';
    return data;
}

static char *lua_mqtt_get_data_or_file(lua_State *L, int table, const char *data_name, const char *file_name)
{
    table = lua_absindex(L, table);
    lua_getfield(L, table, data_name);
    lua_getfield(L, table, file_name);
    if (!lua_isnil(L, -2) && !lua_isnil(L, -1)) luaL_error(L, "%s and %s are mutually exclusive", data_name, file_name);
    char *data = NULL;
    if (!lua_isnil(L, -2)) data = lua_mqtt_dup_lstring(L, -2, NULL);
    if (!lua_isnil(L, -1)) {
        size_t len = 0;
        const char *path = luaL_checklstring(L, -1, &len);
        if (strlen(path) != len) luaL_error(L, "%s must not contain NUL bytes", file_name);
        data = lua_mqtt_read_file(L, path);
    }
    lua_pop(L, 2);
    return data;
}

static lua_mqtt_event_type_t lua_mqtt_event_from_name(const char *name)
{
    for (int i = 0; i < LUA_MQTT_EVENT_MAX; i++) {
        if (strcmp(name, s_event_names[i]) == 0) return (lua_mqtt_event_type_t)i;
    }
    return LUA_MQTT_EVENT_MAX;
}

static const char *lua_mqtt_overflow_name(lua_mqtt_overflow_t reason)
{
    switch (reason) {
    case LUA_MQTT_OVERFLOW_QUEUE: return "event_queue_full";
    case LUA_MQTT_OVERFLOW_MESSAGE_SIZE: return "message_too_large";
    case LUA_MQTT_OVERFLOW_FRAGMENT: return "invalid_fragment";
    case LUA_MQTT_OVERFLOW_MEMORY: return "out_of_memory";
    default: return "unknown";
    }
}

static void lua_mqtt_enqueue(lua_mqtt_client_t *client, lua_mqtt_event_t *event, bool critical)
{
    if (!client || !event || atomic_load(&client->closing) || !client->queue) {
        lua_mqtt_event_free(event);
        return;
    }
    if (xQueueSend(client->queue, &event, 0) == pdTRUE) return;
    atomic_fetch_add(&client->events_dropped, 1);
    if (event->type == LUA_MQTT_EVENT_MESSAGE) atomic_fetch_add(&client->messages_dropped, 1);
    if (critical) {
        lua_mqtt_event_t *dropped = NULL;
        if (xQueueReceive(client->queue, &dropped, 0) == pdTRUE) {
            if (dropped->type == LUA_MQTT_EVENT_MESSAGE) atomic_fetch_add(&client->messages_dropped, 1);
            lua_mqtt_event_free(dropped);
        }
        if (xQueueSend(client->queue, &event, 0) == pdTRUE) return;
    }
    unsigned dropped = atomic_load(&client->events_dropped);
    if (dropped == 1 || dropped % 16 == 0) ESP_LOGW(TAG, "event queue full, dropped=%u", dropped);
    lua_mqtt_event_free(event);
}

static void lua_mqtt_enqueue_overflow(lua_mqtt_client_t *client, lua_mqtt_overflow_t reason)
{
    lua_mqtt_event_t *event = calloc(1, sizeof(*event));
    if (!event) return;
    event->type = LUA_MQTT_EVENT_OVERFLOW;
    event->overflow = reason;
    lua_mqtt_enqueue(client, event, true);
}

static void lua_mqtt_incoming_reset(lua_mqtt_client_t *client)
{
    lua_mqtt_event_free(client->incoming);
    client->incoming = NULL;
}

static void lua_mqtt_handle_data(lua_mqtt_client_t *client, esp_mqtt_event_handle_t source)
{
    /* ESP-MQTT may split one payload across multiple DATA events. */
    if (source->current_data_offset == 0) {
        lua_mqtt_incoming_reset(client);
        if (source->total_data_len < 0 || source->topic_len < 0 || (size_t)source->total_data_len > client->max_message_size) {
            atomic_fetch_add(&client->messages_dropped, 1);
            lua_mqtt_enqueue_overflow(client, LUA_MQTT_OVERFLOW_MESSAGE_SIZE);
            return;
        }
        lua_mqtt_event_t *event = calloc(1, sizeof(*event));
        if (!event) {
            atomic_fetch_add(&client->messages_dropped, 1);
            lua_mqtt_enqueue_overflow(client, LUA_MQTT_OVERFLOW_MEMORY);
            return;
        }
        event->type = LUA_MQTT_EVENT_MESSAGE;
        event->msg_id = source->msg_id;
        event->qos = source->qos;
        event->retain = source->retain;
        event->dup = source->dup;
        event->payload_len = (size_t)source->total_data_len;
        if (source->topic && source->topic_len > 0) {
            event->topic = malloc((size_t)source->topic_len + 1);
            if (event->topic) {
                memcpy(event->topic, source->topic, (size_t)source->topic_len);
                event->topic[source->topic_len] = '\0';
                event->topic_len = (size_t)source->topic_len;
            }
        }
        if (event->payload_len > 0) event->payload = malloc(event->payload_len);
        if ((source->topic_len > 0 && !event->topic) || (event->payload_len > 0 && !event->payload)) {
            lua_mqtt_event_free(event);
            atomic_fetch_add(&client->messages_dropped, 1);
            lua_mqtt_enqueue_overflow(client, LUA_MQTT_OVERFLOW_MEMORY);
            return;
        }
        client->incoming = event;
    }

    lua_mqtt_event_t *event = client->incoming;
    if (!event || source->current_data_offset < 0 || source->data_len < 0 ||
            (size_t)source->current_data_offset != event->received ||
            event->received + (size_t)source->data_len > event->payload_len) {
        lua_mqtt_incoming_reset(client);
        atomic_fetch_add(&client->messages_dropped, 1);
        lua_mqtt_enqueue_overflow(client, LUA_MQTT_OVERFLOW_FRAGMENT);
        return;
    }
    if (source->data_len > 0) memcpy(event->payload + event->received, source->data, (size_t)source->data_len);
    event->received += (size_t)source->data_len;
    if (event->received == event->payload_len) {
        client->incoming = NULL;
        atomic_fetch_add(&client->messages_received, 1);
        lua_mqtt_enqueue(client, event, false);
    }
}

static lua_mqtt_event_t *lua_mqtt_event_new(lua_mqtt_event_type_t type, esp_mqtt_event_handle_t source)
{
    lua_mqtt_event_t *event = calloc(1, sizeof(*event));
    if (!event) return NULL;
    event->type = type;
    event->granted_qos = -1;
    event->msg_id = source->msg_id;
    event->session_present = source->session_present;
    if (type == LUA_MQTT_EVENT_SUBSCRIBED && source->data && source->data_len > 0) event->granted_qos = (uint8_t)source->data[0];
#ifdef CONFIG_MQTT_PROTOCOL_5
    event->reason_code = source->reason_code;
#endif
    if (source->error_handle) {
        event->error_type = source->error_handle->error_type;
        event->connect_return_code = source->error_handle->connect_return_code;
        event->tls_error = source->error_handle->esp_tls_last_esp_err;
        event->tls_stack_error = source->error_handle->esp_tls_stack_err;
        event->cert_verify_flags = source->error_handle->esp_tls_cert_verify_flags;
        event->socket_errno = source->error_handle->esp_transport_sock_errno;
    }
    return event;
}

static void lua_mqtt_native_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    /* Keep all Lua calls on the owning Lua task. */
    (void)base;
    lua_mqtt_client_t *client = arg;
    esp_mqtt_event_handle_t source = event_data;
    if (!client || !source || atomic_load(&client->closing)) return;

    lua_mqtt_event_type_t type;
    bool critical = false;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_BEFORE_CONNECT: type = LUA_MQTT_EVENT_BEFORE_CONNECT; break;
    case MQTT_EVENT_CONNECTED: type = LUA_MQTT_EVENT_CONNECTED; critical = true; break;
    case MQTT_EVENT_DISCONNECTED: type = LUA_MQTT_EVENT_DISCONNECTED; critical = true; break;
    case MQTT_EVENT_SUBSCRIBED: type = LUA_MQTT_EVENT_SUBSCRIBED; break;
    case MQTT_EVENT_UNSUBSCRIBED: type = LUA_MQTT_EVENT_UNSUBSCRIBED; break;
    case MQTT_EVENT_PUBLISHED: type = LUA_MQTT_EVENT_PUBLISHED; break;
    case MQTT_EVENT_DATA: lua_mqtt_handle_data(client, source); return;
    case MQTT_EVENT_ERROR: type = LUA_MQTT_EVENT_ERROR; critical = true; break;
    default: return;
    }
    lua_mqtt_event_t *event = lua_mqtt_event_new(type, source);
    if (event) lua_mqtt_enqueue(client, event, critical);
}

static lua_mqtt_client_ud_t *lua_mqtt_check_ud(lua_State *L, int index)
{
    return luaL_checkudata(L, index, LUA_MQTT_CLIENT_MT);
}

static lua_mqtt_client_t *lua_mqtt_check_client(lua_State *L, int index)
{
    lua_mqtt_client_ud_t *ud = lua_mqtt_check_ud(L, index);
    if (!ud->client || ud->client->closed || atomic_load(&ud->client->closing)) luaL_error(L, "mqtt client is closed");
    return ud->client;
}

static int lua_mqtt_push_error(lua_State *L, const char *operation, esp_err_t err)
{
    lua_pushnil(L);
    lua_pushfstring(L, "%s failed: %s", operation, esp_err_to_name(err));
    return 2;
}

static void lua_mqtt_close_now(lua_State *L, lua_mqtt_client_t *client)
{
    if (!client || client->closed) return;
    atomic_store(&client->closing, true);
    client->closed = true;
    if (client->native) {
        (void)esp_mqtt_client_unregister_event(client->native, ESP_EVENT_ANY_ID, lua_mqtt_native_event);
        esp_err_t err = esp_mqtt_client_destroy(client->native);
        if (err != ESP_OK) ESP_LOGE(TAG, "client destroy failed: %s", esp_err_to_name(err));
        client->native = NULL;
    }
    lua_mqtt_incoming_reset(client);
    if (client->queue) {
        lua_mqtt_event_t *event = NULL;
        while (xQueueReceive(client->queue, &event, 0) == pdTRUE) lua_mqtt_event_free(event);
        vQueueDelete(client->queue);
        client->queue = NULL;
    }
    for (int i = 0; i < LUA_MQTT_EVENT_MAX; i++) {
        if (client->callback_refs[i] != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, client->callback_refs[i]);
    }
    lua_mqtt_unlink(L, client);
    lua_mqtt_owned_config_free(&client->owned);
    if (client->owner_ud) client->owner_ud->client = NULL;
    free(client);
}

static int lua_mqtt_client_close(lua_State *L)
{
    lua_mqtt_client_ud_t *ud = lua_mqtt_check_ud(L, 1);
    if (ud->client) {
        if (ud->client->dispatch_depth > 0) {
            ud->client->close_requested = true;
            atomic_store(&ud->client->closing, true);
        } else {
            lua_mqtt_close_now(L, ud->client);
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_mqtt_client_gc(lua_State *L)
{
    lua_mqtt_client_ud_t *ud = luaL_testudata(L, 1, LUA_MQTT_CLIENT_MT);
    if (ud && ud->client) lua_mqtt_close_now(L, ud->client);
    return 0;
}

static int lua_mqtt_client_start(lua_State *L)
{
    lua_mqtt_client_t *client = lua_mqtt_check_client(L, 1);
    esp_err_t err = esp_mqtt_client_start(client->native);
    if (err != ESP_OK) return lua_mqtt_push_error(L, "mqtt start", err);
    client->started = true;
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_mqtt_client_stop(lua_State *L)
{
    lua_mqtt_client_t *client = lua_mqtt_check_client(L, 1);
    esp_err_t err = esp_mqtt_client_stop(client->native);
    if (err != ESP_OK) return lua_mqtt_push_error(L, "mqtt stop", err);
    client->started = false;
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_mqtt_client_disconnect(lua_State *L)
{
    lua_mqtt_client_t *client = lua_mqtt_check_client(L, 1);
    esp_err_t err = esp_mqtt_client_disconnect(client->native);
    if (err != ESP_OK) return lua_mqtt_push_error(L, "mqtt disconnect", err);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_mqtt_client_reconnect(lua_State *L)
{
    lua_mqtt_client_t *client = lua_mqtt_check_client(L, 1);
    esp_err_t err = esp_mqtt_client_reconnect(client->native);
    if (err != ESP_OK) return lua_mqtt_push_error(L, "mqtt reconnect", err);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_mqtt_client_publish(lua_State *L)
{
    lua_mqtt_client_t *client = lua_mqtt_check_client(L, 1);
    size_t topic_len = 0;
    const char *topic = luaL_checklstring(L, 2, &topic_len);
    size_t payload_len = 0;
    const char *payload = luaL_checklstring(L, 3, &payload_len);
    int qos = 0;
    bool retain = false;
    bool enqueue = true;
    bool store = false;
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TTABLE);
        qos = lua_mqtt_get_integer_field(L, 4, "qos", 0);
        retain = lua_mqtt_get_boolean_field(L, 4, "retain", false);
        enqueue = lua_mqtt_get_boolean_field(L, 4, "enqueue", true);
        store = lua_mqtt_get_boolean_field(L, 4, "store", false);
    }
    if (topic_len == 0 || strlen(topic) != topic_len) return luaL_error(L, "topic must be a non-empty string without NUL bytes");
    if (payload_len > INT_MAX) return luaL_error(L, "payload is too large");
    if (qos < 0 || qos > 2) return luaL_error(L, "qos must be 0, 1, or 2");
    int msg_id = enqueue ? esp_mqtt_client_enqueue(client->native, topic, payload, (int)payload_len, qos, retain, store)
                         : esp_mqtt_client_publish(client->native, topic, payload, (int)payload_len, qos, retain);
    if (msg_id < 0) {
        lua_pushnil(L);
        lua_pushstring(L, msg_id == -2 ? "mqtt outbox full" : "mqtt publish failed");
        return 2;
    }
    lua_pushinteger(L, msg_id);
    return 1;
}

static int lua_mqtt_client_subscribe(lua_State *L)
{
    lua_mqtt_client_t *client = lua_mqtt_check_client(L, 1);
    size_t topic_len = 0;
    const char *topic = luaL_checklstring(L, 2, &topic_len);
    int qos = 0;
    if (lua_isinteger(L, 3)) qos = (int)lua_tointeger(L, 3);
    else if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        qos = lua_mqtt_get_integer_field(L, 3, "qos", 0);
    }
    if (topic_len == 0 || strlen(topic) != topic_len) return luaL_error(L, "topic filter must be a non-empty string without NUL bytes");
    if (qos < 0 || qos > 2) return luaL_error(L, "qos must be 0, 1, or 2");
    int msg_id = esp_mqtt_client_subscribe_single(client->native, topic, qos);
    if (msg_id < 0) {
        lua_pushnil(L);
        lua_pushstring(L, msg_id == -2 ? "mqtt outbox full" : "mqtt subscribe failed");
        return 2;
    }
    lua_pushinteger(L, msg_id);
    return 1;
}

static int lua_mqtt_client_unsubscribe(lua_State *L)
{
    lua_mqtt_client_t *client = lua_mqtt_check_client(L, 1);
    size_t topic_len = 0;
    const char *topic = luaL_checklstring(L, 2, &topic_len);
    if (topic_len == 0 || strlen(topic) != topic_len) return luaL_error(L, "topic filter must be a non-empty string without NUL bytes");
    int msg_id = esp_mqtt_client_unsubscribe(client->native, topic);
    if (msg_id < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "mqtt unsubscribe failed");
        return 2;
    }
    lua_pushinteger(L, msg_id);
    return 1;
}

static int lua_mqtt_client_on(lua_State *L)
{
    lua_mqtt_client_t *client = lua_mqtt_check_client(L, 1);
    const char *name = luaL_checkstring(L, 2);
    lua_mqtt_event_type_t type = lua_mqtt_event_from_name(name);
    if (type == LUA_MQTT_EVENT_MAX) return luaL_error(L, "unknown mqtt event: %s", name);
    if (!lua_isnil(L, 3)) luaL_checktype(L, 3, LUA_TFUNCTION);
    if (client->callback_refs[type] != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, client->callback_refs[type]);
        client->callback_refs[type] = LUA_NOREF;
    }
    if (!lua_isnil(L, 3)) {
        lua_pushvalue(L, 3);
        client->callback_refs[type] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_settop(L, 1);
    return 1;
}

static void lua_mqtt_set_integer(lua_State *L, const char *name, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, name);
}

static void lua_mqtt_set_boolean(lua_State *L, const char *name, bool value)
{
    lua_pushboolean(L, value);
    lua_setfield(L, -2, name);
}

static void lua_mqtt_push_event(lua_State *L, const lua_mqtt_event_t *event)
{
    lua_newtable(L);
    lua_pushstring(L, s_event_names[event->type]);
    lua_setfield(L, -2, "type");
    if (event->msg_id) lua_mqtt_set_integer(L, "packet_id", event->msg_id);
    if (event->reason_code) lua_mqtt_set_integer(L, "reason_code", event->reason_code);
    switch (event->type) {
    case LUA_MQTT_EVENT_CONNECTED:
        lua_mqtt_set_boolean(L, "session_present", event->session_present != 0);
        break;
    case LUA_MQTT_EVENT_SUBSCRIBED:
        if (event->granted_qos >= 0) lua_mqtt_set_integer(L, "granted_qos", event->granted_qos);
        break;
    case LUA_MQTT_EVENT_MESSAGE:
        lua_pushlstring(L, event->topic ? event->topic : "", event->topic_len);
        lua_setfield(L, -2, "topic");
        lua_pushlstring(L, event->payload ? event->payload : "", event->payload_len);
        lua_setfield(L, -2, "payload");
        lua_mqtt_set_integer(L, "qos", event->qos);
        lua_mqtt_set_boolean(L, "retain", event->retain);
        lua_mqtt_set_boolean(L, "dup", event->dup);
        break;
    case LUA_MQTT_EVENT_ERROR:
        lua_mqtt_set_integer(L, "error_type", event->error_type);
        lua_mqtt_set_integer(L, "connect_return_code", event->connect_return_code);
        lua_mqtt_set_integer(L, "esp_err", event->tls_error);
        lua_mqtt_set_integer(L, "tls_error", event->tls_stack_error);
        lua_mqtt_set_integer(L, "cert_verify_flags", event->cert_verify_flags);
        lua_mqtt_set_integer(L, "socket_errno", event->socket_errno);
        break;
    case LUA_MQTT_EVENT_OVERFLOW:
        lua_pushstring(L, lua_mqtt_overflow_name(event->overflow));
        lua_setfield(L, -2, "reason");
        break;
    default:
        break;
    }
}

static int lua_mqtt_dispatch(lua_State *L, lua_mqtt_client_t *client, int timeout_ms, int max_events, bool *closed)
{
    int processed = 0;
    *closed = false;
    while (processed < max_events && client->queue) {
        lua_mqtt_event_t *event = NULL;
        TickType_t wait = processed == 0 && timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : 0;
        if (xQueueReceive(client->queue, &event, wait) != pdTRUE) break;
        int callback_ref = client->callback_refs[event->type];
        if (callback_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, callback_ref);
            lua_mqtt_push_event(L, event);
            client->dispatch_depth++;
            int status = lua_pcall(L, 1, 0, 0);
            client->dispatch_depth--;
            if (status != LUA_OK) {
                const char *message = lua_tostring(L, -1);
                ESP_LOGE(TAG, "event callback failed type=%s: %s", s_event_names[event->type], message ? message : "unknown");
                atomic_fetch_add(&client->callback_errors, 1);
                lua_pop(L, 1);
            }
        }
        lua_mqtt_event_free(event);
        processed++;
        if (client->close_requested) {
            lua_mqtt_close_now(L, client);
            *closed = true;
            break;
        }
    }
    return processed;
}

static int lua_mqtt_client_process_events(lua_State *L)
{
    lua_mqtt_client_t *client = lua_mqtt_check_client(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 0);
    int max_events = (int)luaL_optinteger(L, 3, LUA_MQTT_PROCESS_MAX);
    if (timeout_ms < 0) return luaL_error(L, "timeout_ms must be >= 0");
    if (max_events < 1 || max_events > 64) return luaL_error(L, "max_events must be in 1..64");
    bool closed = false;
    int processed = lua_mqtt_dispatch(L, client, timeout_ms, max_events, &closed);
    lua_pushinteger(L, processed);
    return 1;
}

static int lua_mqtt_client_run(lua_State *L)
{
    lua_mqtt_client_t *client = lua_mqtt_check_client(L, 1);
    int poll_ms = 200;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        poll_ms = lua_mqtt_get_integer_field(L, 2, "poll_ms", 200);
    }
    if (poll_ms < 10 || poll_ms > 10000) return luaL_error(L, "poll_ms must be in 10..10000");
    while (!cap_lua_runtime_stop_requested(L)) {
        bool closed = false;
        (void)lua_mqtt_dispatch(L, client, poll_ms, LUA_MQTT_PROCESS_MAX, &closed);
        if (closed) break;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const char *lua_mqtt_state_name(esp_mqtt_client_connection_state_t state)
{
    switch (state) {
    case MQTT_CLIENT_STATE_NOT_INITIALIZED: return "not_initialized";
    case MQTT_CLIENT_STATE_NOT_STARTED: return "not_started";
    case MQTT_CLIENT_STATE_DISCONNECTED: return "disconnected";
    case MQTT_CLIENT_STATE_CONNECTED: return "connected";
    case MQTT_CLIENT_STATE_WAITING_RECONNECT: return "waiting_reconnect";
    default: return "unknown";
    }
}

static int lua_mqtt_client_state(lua_State *L)
{
    lua_mqtt_client_t *client = lua_mqtt_check_client(L, 1);
    lua_pushstring(L, lua_mqtt_state_name(esp_mqtt_client_get_state(client->native)));
    return 1;
}

static int lua_mqtt_client_stats(lua_State *L)
{
    lua_mqtt_client_t *client = lua_mqtt_check_client(L, 1);
    lua_newtable(L);
    lua_mqtt_set_integer(L, "events_dropped", atomic_load(&client->events_dropped));
    lua_mqtt_set_integer(L, "messages_dropped", atomic_load(&client->messages_dropped));
    lua_mqtt_set_integer(L, "callback_errors", atomic_load(&client->callback_errors));
    lua_mqtt_set_integer(L, "messages_received", atomic_load(&client->messages_received));
    lua_mqtt_set_integer(L, "outbox_bytes", esp_mqtt_client_get_outbox_size(client->native));
    if (client->queue) lua_mqtt_set_integer(L, "queued_events", uxQueueMessagesWaiting(client->queue));
    lua_pushstring(L, lua_mqtt_state_name(esp_mqtt_client_get_state(client->native)));
    lua_setfield(L, -2, "state");
    return 1;
}

static bool lua_mqtt_uri_is_secure(const char *uri)
{
    return strncmp(uri, "mqtts://", 8) == 0 || strncmp(uri, "wss://", 6) == 0;
}

static bool lua_mqtt_uri_is_supported(const char *uri)
{
    return strncmp(uri, "mqtt://", 7) == 0 || strncmp(uri, "mqtts://", 8) == 0 ||
           strncmp(uri, "ws://", 5) == 0 || strncmp(uri, "wss://", 6) == 0;
}

static void lua_mqtt_parse_tls(lua_State *L, int table, lua_mqtt_client_t *client, esp_mqtt_client_config_t *config)
{
    bool secure = lua_mqtt_uri_is_secure(client->owned.uri);
    bool use_bundle = secure;
    bool insecure = false;
    bool skip_common_name_check = false;
    lua_getfield(L, table, "tls");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        int tls = lua_gettop(L);
        use_bundle = lua_mqtt_get_boolean_field(L, tls, "use_crt_bundle", secure);
        insecure = lua_mqtt_get_boolean_field(L, tls, "insecure", false);
        skip_common_name_check = lua_mqtt_get_boolean_field(L, tls, "skip_common_name_check", false);
        client->owned.ca = lua_mqtt_get_data_or_file(L, tls, "ca", "ca_file");
        client->owned.client_cert = lua_mqtt_get_data_or_file(L, tls, "client_cert", "client_cert_file");
        client->owned.client_key = lua_mqtt_get_data_or_file(L, tls, "client_key", "client_key_file");
    }
    lua_pop(L, 1);
    if (!secure && (client->owned.ca || client->owned.client_cert || client->owned.client_key)) {
        luaL_error(L, "TLS credentials require mqtts:// or wss:// URI");
    }
    if ((client->owned.client_cert == NULL) != (client->owned.client_key == NULL)) {
        luaL_error(L, "client_cert and client_key must be configured together");
    }
    if (secure && !insecure && !use_bundle && !client->owned.ca) {
        luaL_error(L, "secure MQTT requires certificate verification");
    }
    if (client->owned.ca) config->broker.verification.certificate = client->owned.ca;
    else if (secure && use_bundle && !insecure) config->broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    if (insecure || skip_common_name_check) config->broker.verification.skip_cert_common_name_check = true;
    config->credentials.authentication.certificate = client->owned.client_cert;
    config->credentials.authentication.key = client->owned.client_key;
}

static int lua_mqtt_client_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_mqtt_client_t *client = calloc(1, sizeof(*client));
    if (!client) return luaL_error(L, "out of memory");
    client->L = L;
    client->max_message_size = LUA_MQTT_DEFAULT_MESSAGE_MAX;
    atomic_init(&client->closing, false);
    atomic_init(&client->events_dropped, 0);
    atomic_init(&client->messages_dropped, 0);
    atomic_init(&client->callback_errors, 0);
    atomic_init(&client->messages_received, 0);
    for (int i = 0; i < LUA_MQTT_EVENT_MAX; i++) client->callback_refs[i] = LUA_NOREF;

    lua_mqtt_client_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    ud->client = client;
    client->owner_ud = ud;
    luaL_setmetatable(L, LUA_MQTT_CLIENT_MT);
    lua_mqtt_state_t *state = lua_mqtt_state(L);
    client->next = state->clients;
    state->clients = client;

    esp_mqtt_client_config_t config = {0};
    int queue_len = LUA_MQTT_DEFAULT_QUEUE_LEN;
    client->owned.uri = lua_mqtt_get_string_field(L, 1, "uri");
    client->owned.client_id = lua_mqtt_get_string_field(L, 1, "client_id");
    client->owned.username = lua_mqtt_get_string_field(L, 1, "username");
    client->owned.password = lua_mqtt_get_string_field(L, 1, "password");
    if (!client->owned.uri || !lua_mqtt_uri_is_supported(client->owned.uri)) {
        return luaL_error(L, "uri must use mqtt://, mqtts://, ws://, or wss://");
    }
    char *protocol = lua_mqtt_get_string_field(L, 1, "protocol");
    if (protocol && strcmp(protocol, "3.1.1") != 0) {
        free(protocol);
        return luaL_error(L, "only MQTT 3.1.1 is enabled in this build");
    }
    free(protocol);

    config.broker.address.uri = client->owned.uri;
    config.credentials.client_id = client->owned.client_id;
    config.credentials.username = client->owned.username;
    config.credentials.authentication.password = client->owned.password;
    config.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;

    lua_getfield(L, 1, "session");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        int session = lua_gettop(L);
        config.session.disable_clean_session = !lua_mqtt_get_boolean_field(L, session, "clean_start", true);
        config.session.keepalive = lua_mqtt_get_integer_field(L, session, "keep_alive", 60);
        if (config.session.keepalive < 10 || config.session.keepalive > 3600) luaL_error(L, "keep_alive must be in 10..3600");
    } else {
        config.session.keepalive = 60;
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "reconnect");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        int reconnect = lua_gettop(L);
        config.network.disable_auto_reconnect = !lua_mqtt_get_boolean_field(L, reconnect, "enabled", true);
        config.network.reconnect_timeout_ms = lua_mqtt_get_integer_field(L, reconnect, "interval_ms", 10000);
        if (config.network.reconnect_timeout_ms < 100 || config.network.reconnect_timeout_ms > 3600000) {
            luaL_error(L, "reconnect interval_ms must be in 100..3600000");
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "will");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        int will = lua_gettop(L);
        client->owned.will_topic = lua_mqtt_get_string_field(L, will, "topic");
        lua_getfield(L, will, "payload");
        if (!lua_isnil(L, -1)) client->owned.will_payload = lua_mqtt_dup_lstring(L, -1, &client->owned.will_payload_len);
        lua_pop(L, 1);
        if (!client->owned.will_topic) luaL_error(L, "will.topic is required");
        config.session.last_will.topic = client->owned.will_topic;
        config.session.last_will.msg = client->owned.will_payload;
        config.session.last_will.msg_len = (int)client->owned.will_payload_len;
        config.session.last_will.qos = lua_mqtt_get_integer_field(L, will, "qos", 0);
        config.session.last_will.retain = lua_mqtt_get_boolean_field(L, will, "retain", false);
        if (config.session.last_will.qos < 0 || config.session.last_will.qos > 2) luaL_error(L, "will.qos must be 0, 1, or 2");
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "limits");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        int limits = lua_gettop(L);
        queue_len = lua_mqtt_get_integer_field(L, limits, "event_queue", LUA_MQTT_DEFAULT_QUEUE_LEN);
        client->max_message_size = (size_t)lua_mqtt_get_integer_field(L, limits, "max_message_size", LUA_MQTT_DEFAULT_MESSAGE_MAX);
        config.buffer.size = lua_mqtt_get_integer_field(L, limits, "mqtt_buffer_size", 0);
        config.outbox.limit = (uint64_t)lua_mqtt_get_integer_field(L, limits, "outbox_size", 0);
        if (queue_len < 4 || queue_len > 64) luaL_error(L, "event_queue must be in 4..64");
        if (client->max_message_size < 256 || client->max_message_size > 1048576) luaL_error(L, "max_message_size must be in 256..1048576");
        if (config.buffer.size && (config.buffer.size < 512 || config.buffer.size > 16384)) luaL_error(L, "mqtt_buffer_size must be in 512..16384");
        if (config.outbox.limit && (config.outbox.limit < 1024 || config.outbox.limit > 1048576)) luaL_error(L, "outbox_size must be in 1024..1048576");
    }
    lua_pop(L, 1);
    lua_mqtt_parse_tls(L, 1, client, &config);

    client->queue = xQueueCreate((UBaseType_t)queue_len, sizeof(lua_mqtt_event_t *));
    if (!client->queue) goto fail;
    client->native = esp_mqtt_client_init(&config);
    if (!client->native) goto fail;
    if (esp_mqtt_client_register_event(client->native, ESP_EVENT_ANY_ID, lua_mqtt_native_event, client) != ESP_OK) goto fail;

    lua_pushvalue(L, 2);
    return 1;

fail:
    lua_mqtt_close_now(L, client);
    lua_pushnil(L);
    lua_pushstring(L, "failed to create mqtt client");
    return 2;
}

static void lua_mqtt_exit_cleanup(lua_State *L)
{
    lua_mqtt_state_t *state = lua_mqtt_state(L);
    if (!state) return;
    while (state->clients) lua_mqtt_close_now(L, state->clients);
}

int luaopen_mqtt(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"start", lua_mqtt_client_start}, {"stop", lua_mqtt_client_stop},
        {"disconnect", lua_mqtt_client_disconnect}, {"reconnect", lua_mqtt_client_reconnect},
        {"publish", lua_mqtt_client_publish}, {"subscribe", lua_mqtt_client_subscribe},
        {"unsubscribe", lua_mqtt_client_unsubscribe}, {"on", lua_mqtt_client_on},
        {"process_events", lua_mqtt_client_process_events}, {"run", lua_mqtt_client_run},
        {"state", lua_mqtt_client_state}, {"stats", lua_mqtt_client_stats},
        {"close", lua_mqtt_client_close}, {NULL, NULL},
    };
    if (luaL_newmetatable(L, LUA_MQTT_CLIENT_MT)) {
        lua_pushcfunction(L, lua_mqtt_client_gc);
        lua_setfield(L, -2, "__gc");
        lua_newtable(L);
        luaL_setfuncs(L, methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    lua_rawgetp(L, LUA_REGISTRYINDEX, &s_state_key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newuserdata(L, sizeof(lua_mqtt_state_t));
        memset(lua_touserdata(L, -1), 0, sizeof(lua_mqtt_state_t));
        lua_rawsetp(L, LUA_REGISTRYINDEX, &s_state_key);
    } else {
        lua_pop(L, 1);
    }

    lua_newtable(L);
    lua_pushcfunction(L, lua_mqtt_client_new);
    lua_setfield(L, -2, "client");
    return 1;
}

esp_err_t lua_module_mqtt_register(void)
{
    esp_err_t err = cap_lua_register_module("mqtt", luaopen_mqtt);
    return err == ESP_OK ? cap_lua_register_exit_cleanup(lua_mqtt_exit_cleanup) : err;
}
