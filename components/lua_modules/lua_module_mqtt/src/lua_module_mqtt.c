/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_mqtt.h"

#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "lauxlib.h"
#include "mqtt_client.h"

#define LUA_MODULE_MQTT_METATABLE     "mqtt"
#define LUA_MODULE_MQTT_TAG           "lua_mqtt"
#define LUA_MODULE_MQTT_DEFAULT_QLEN  16
#define LUA_MODULE_MQTT_MAX_QLEN      128
#define LUA_MODULE_MQTT_CONNECTED_BIT BIT0
#define LUA_MODULE_MQTT_MAX_CALLBACKS 16

/* Message copied out of the esp-mqtt event task and handed to the Lua task. */
typedef struct {
    char *topic;
    char *payload;
    size_t payload_len;
} lua_module_mqtt_rx_msg_t;

/* One registered topic-pattern callback. `pattern` is the strdup'd subscription
 * pattern (the registration key); `lua_ref` is a luaL_ref into the registry, or
 * LUA_NOREF when the slot is free. Stored per-client (see decision below). */
typedef struct {
    char *pattern;
    int   lua_ref;
} lua_module_mqtt_cb_reg_t;

typedef struct {
    esp_mqtt_client_handle_t client;
    QueueHandle_t rx_queue; /* holds lua_module_mqtt_rx_msg_t* */
    EventGroupHandle_t state;
    bool started;
    /* Callback registry lives inside the userdata (per-client), not in a global
     * array: it is freed deterministically when the client is destroyed. */
    lua_module_mqtt_cb_reg_t callbacks[LUA_MODULE_MQTT_MAX_CALLBACKS];
} lua_module_mqtt_ud_t;

/* Default broker connection values injected by the application layer. NULL
 * means "no default"; explicit `mqtt.new()` arguments always take priority. */
static char *s_default_uri;
static char *s_default_username;
static char *s_default_password;
static char *s_default_client_id;

static char *lua_module_mqtt_dup_or_null(const char *value)
{
    if (!value || !value[0]) {
        return NULL;
    }
    return strdup(value);
}

esp_err_t lua_module_mqtt_set_defaults(const char *uri,
                                       const char *username,
                                       const char *password,
                                       const char *client_id)
{
    char *new_uri       = lua_module_mqtt_dup_or_null(uri);
    char *new_username  = lua_module_mqtt_dup_or_null(username);
    char *new_password  = lua_module_mqtt_dup_or_null(password);
    char *new_client_id = lua_module_mqtt_dup_or_null(client_id);

    if ((uri       && uri[0]       && !new_uri)       ||
        (username  && username[0]  && !new_username)  ||
        (password  && password[0]  && !new_password)  ||
        (client_id && client_id[0] && !new_client_id)) {
        free(new_uri);
        free(new_username);
        free(new_password);
        free(new_client_id);
        return ESP_ERR_NO_MEM;
    }

    free(s_default_uri);
    free(s_default_username);
    free(s_default_password);
    free(s_default_client_id);
    s_default_uri       = new_uri;
    s_default_username  = new_username;
    s_default_password  = new_password;
    s_default_client_id = new_client_id;
    return ESP_OK;
}

static void lua_module_mqtt_rx_msg_free(lua_module_mqtt_rx_msg_t *msg)
{
    if (!msg) {
        return;
    }
    free(msg->topic);
    free(msg->payload);
    free(msg);
}

/* Drain and free every queued message. Safe to call when no producer runs. */
static void lua_module_mqtt_rx_queue_flush(QueueHandle_t queue)
{
    if (!queue) {
        return;
    }
    lua_module_mqtt_rx_msg_t *msg = NULL;
    while (xQueueReceive(queue, &msg, 0) == pdTRUE) {
        lua_module_mqtt_rx_msg_free(msg);
    }
}

/* MQTT topic-filter match. `pattern` is a subscription filter that may contain
 * wildcards; `topic` is a concrete published topic (no wildcards). Rules:
 *   '+'  matches exactly one level (the text between '/' separators);
 *   '#'  matches the remaining levels and must be the last pattern level; it
 *        also matches zero levels, so "sport/#" matches "sport" (MQTT spec).
 * Comparison walks both strings level by level without allocating. Returns
 * true on full match. */
static bool lua_module_mqtt_topic_matches(const char *pattern, const char *topic)
{
    const char *p = pattern;
    const char *t = topic;

    while (*p) {
        if (p[0] == '#' && (p[1] == '\0')) {
            /* Trailing '#' swallows the rest, including zero remaining levels. */
            return true;
        }

        /* Bounds of the current pattern level and topic level. */
        const char *p_end = p;
        while (*p_end && *p_end != '/') {
            p_end++;
        }
        const char *t_end = t;
        while (*t_end && *t_end != '/') {
            t_end++;
        }

        bool single = (p_end - p == 1 && p[0] == '+');
        if (!single) {
            size_t p_len = (size_t)(p_end - p);
            size_t t_len = (size_t)(t_end - t);
            if (p_len != t_len || strncmp(p, t, p_len) != 0) {
                return false;
            }
        }

        p = p_end;
        t = t_end;
        if (*p == '/') {
            p++;
        }
        if (*t == '/') {
            t++;
        }
        /* Pattern level remains but topic is exhausted: only a lone trailing
         * '#' (handled at loop top) may match an empty tail. */
        if (*p && !*t) {
            if (p[0] == '#' && p[1] == '\0') {
                return true;
            }
            return false;
        }
    }

    /* Both must be fully consumed for an exact-length match. */
    return *t == '\0';
}

/* Release every registered callback: unref the Lua function and free the
 * pattern string. Required from close()/__gc to avoid leaking registry refs. */
static void lua_module_mqtt_free_callbacks(lua_State *L, lua_module_mqtt_ud_t *ud)
{
    for (int i = 0; i < LUA_MODULE_MQTT_MAX_CALLBACKS; i++) {
        if (ud->callbacks[i].lua_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ud->callbacks[i].lua_ref);
            ud->callbacks[i].lua_ref = LUA_NOREF;
        }
        free(ud->callbacks[i].pattern);
        ud->callbacks[i].pattern = NULL;
    }
}

/* Runs on the esp-mqtt task. Never call into the Lua state from here. */
static void lua_module_mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                          int32_t event_id, void *event_data)
{
    (void)base;
    lua_module_mqtt_ud_t *ud = (lua_module_mqtt_ud_t *)handler_args;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        xEventGroupSetBits(ud->state, LUA_MODULE_MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DISCONNECTED:
        xEventGroupClearBits(ud->state, LUA_MODULE_MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DATA: {
        /* Only the first fragment carries the topic. Larger-than-buffer
         * payloads arrive fragmented; this binding keeps single-fragment
         * messages and drops continuation fragments. */
        if (event->topic_len <= 0 || event->current_data_offset != 0) {
            break;
        }
        lua_module_mqtt_rx_msg_t *msg = calloc(1, sizeof(*msg));
        if (!msg) {
            break;
        }
        msg->topic = malloc(event->topic_len + 1);
        msg->payload = malloc(event->data_len + 1);
        if (!msg->topic || !msg->payload) {
            lua_module_mqtt_rx_msg_free(msg);
            break;
        }
        memcpy(msg->topic, event->topic, event->topic_len);
        msg->topic[event->topic_len] = '\0';
        memcpy(msg->payload, event->data, event->data_len);
        msg->payload[event->data_len] = '\0';
        msg->payload_len = (size_t)event->data_len;
        if (xQueueSend(ud->rx_queue, &msg, 0) != pdTRUE) {
            ESP_LOGW(LUA_MODULE_MQTT_TAG, "rx queue full, dropping message on %s", msg->topic);
            lua_module_mqtt_rx_msg_free(msg);
        }
        break;
    }
    default:
        break;
    }
}

static lua_module_mqtt_ud_t *lua_module_mqtt_get_ud(lua_State *L, int idx)
{
    lua_module_mqtt_ud_t *ud =
        (lua_module_mqtt_ud_t *)luaL_checkudata(L, idx, LUA_MODULE_MQTT_METATABLE);
    if (!ud || !ud->client) {
        luaL_error(L, "mqtt: invalid or closed handle");
    }
    return ud;
}

static void lua_module_mqtt_destroy(lua_module_mqtt_ud_t *ud)
{
    if (ud->client) {
        if (ud->started) {
            esp_mqtt_client_stop(ud->client);
            ud->started = false;
        }
        esp_mqtt_client_destroy(ud->client);
        ud->client = NULL;
    }
    if (ud->rx_queue) {
        lua_module_mqtt_rx_queue_flush(ud->rx_queue);
        vQueueDelete(ud->rx_queue);
        ud->rx_queue = NULL;
    }
    if (ud->state) {
        vEventGroupDelete(ud->state);
        ud->state = NULL;
    }
}

static int lua_module_mqtt_gc(lua_State *L)
{
    lua_module_mqtt_ud_t *ud =
        (lua_module_mqtt_ud_t *)luaL_testudata(L, 1, LUA_MODULE_MQTT_METATABLE);
    if (ud) {
        lua_module_mqtt_free_callbacks(L, ud);
        lua_module_mqtt_destroy(ud);
    }
    return 0;
}

static int lua_module_mqtt_close(lua_State *L)
{
    lua_module_mqtt_ud_t *ud =
        (lua_module_mqtt_ud_t *)luaL_checkudata(L, 1, LUA_MODULE_MQTT_METATABLE);
    lua_module_mqtt_free_callbacks(L, ud);
    lua_module_mqtt_destroy(ud);
    return 0;
}

/* connect([timeout_ms]) -> true | false. Starts the client and waits for the
 * broker handshake up to timeout_ms (default 10000). */
static int lua_module_mqtt_connect(lua_State *L)
{
    lua_module_mqtt_ud_t *ud = lua_module_mqtt_get_ud(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 10000);

    if (!ud->started) {
        esp_err_t err = esp_mqtt_client_start(ud->client);
        if (err != ESP_OK) {
            return luaL_error(L, "mqtt connect failed: %s", esp_err_to_name(err));
        }
        ud->started = true;
    }

    EventBits_t bits = xEventGroupWaitBits(ud->state, LUA_MODULE_MQTT_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    lua_pushboolean(L, (bits & LUA_MODULE_MQTT_CONNECTED_BIT) != 0);
    return 1;
}

static int lua_module_mqtt_is_connected(lua_State *L)
{
    lua_module_mqtt_ud_t *ud = lua_module_mqtt_get_ud(L, 1);
    EventBits_t bits = xEventGroupGetBits(ud->state);
    lua_pushboolean(L, (bits & LUA_MODULE_MQTT_CONNECTED_BIT) != 0);
    return 1;
}

/* publish(topic, payload[, qos[, retain]]) -> msg_id */
static int lua_module_mqtt_publish(lua_State *L)
{
    lua_module_mqtt_ud_t *ud = lua_module_mqtt_get_ud(L, 1);
    const char *topic = luaL_checkstring(L, 2);
    size_t payload_len = 0;
    const char *payload = luaL_checklstring(L, 3, &payload_len);
    int qos = (int)luaL_optinteger(L, 4, 0);
    int retain = lua_toboolean(L, 5);

    int msg_id = esp_mqtt_client_publish(ud->client, topic, payload, (int)payload_len, qos, retain);
    if (msg_id < 0) {
        return luaL_error(L, "mqtt publish failed");
    }
    lua_pushinteger(L, msg_id);
    return 1;
}

/* subscribe(topic[, qos]) -> msg_id */
static int lua_module_mqtt_subscribe(lua_State *L)
{
    lua_module_mqtt_ud_t *ud = lua_module_mqtt_get_ud(L, 1);
    const char *topic = luaL_checkstring(L, 2);
    int qos = (int)luaL_optinteger(L, 3, 0);

    int msg_id = esp_mqtt_client_subscribe(ud->client, topic, qos);
    if (msg_id < 0) {
        return luaL_error(L, "mqtt subscribe failed");
    }
    lua_pushinteger(L, msg_id);
    return 1;
}

/* unsubscribe(topic) -> msg_id */
static int lua_module_mqtt_unsubscribe(lua_State *L)
{
    lua_module_mqtt_ud_t *ud = lua_module_mqtt_get_ud(L, 1);
    const char *topic = luaL_checkstring(L, 2);

    int msg_id = esp_mqtt_client_unsubscribe(ud->client, topic);
    if (msg_id < 0) {
        return luaL_error(L, "mqtt unsubscribe failed");
    }
    lua_pushinteger(L, msg_id);
    return 1;
}

/* poll([timeout_ms]) -> {topic=, payload=} | nil. Pulls one received message
 * from the rx queue, blocking up to timeout_ms (default 0, non-blocking). */
static int lua_module_mqtt_poll(lua_State *L)
{
    lua_module_mqtt_ud_t *ud = lua_module_mqtt_get_ud(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 0);

    lua_module_mqtt_rx_msg_t *msg = NULL;
    if (xQueueReceive(ud->rx_queue, &msg, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    lua_pushstring(L, msg->topic);
    lua_setfield(L, -2, "topic");
    lua_pushlstring(L, msg->payload, msg->payload_len);
    lua_setfield(L, -2, "payload");
    lua_module_mqtt_rx_msg_free(msg);
    return 1;
}

/* on(topic, fn) -> true. Registers a Lua function for a topic filter (wildcards
 * allowed). Re-registering the same pattern replaces the previous function.
 * Callbacks fire from dispatch(), which shares the rx queue with poll(): use one
 * model or the other on a given client, not both. */
static int lua_module_mqtt_on(lua_State *L)
{
    lua_module_mqtt_ud_t *ud = lua_module_mqtt_get_ud(L, 1);
    const char *topic = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    /* Replace an existing registration for the same pattern. */
    int slot = -1;
    for (int i = 0; i < LUA_MODULE_MQTT_MAX_CALLBACKS; i++) {
        if (ud->callbacks[i].pattern && strcmp(ud->callbacks[i].pattern, topic) == 0) {
            slot = i;
            break;
        }
    }
    /* Otherwise take the first free slot. */
    if (slot < 0) {
        for (int i = 0; i < LUA_MODULE_MQTT_MAX_CALLBACKS; i++) {
            if (ud->callbacks[i].lua_ref == LUA_NOREF && !ud->callbacks[i].pattern) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        return luaL_error(L, "mqtt on: too many callbacks (max %d)",
                          LUA_MODULE_MQTT_MAX_CALLBACKS);
    }

    if (!ud->callbacks[slot].pattern) {
        char *dup = strdup(topic);
        if (!dup) {
            return luaL_error(L, "mqtt on: out of memory");
        }
        ud->callbacks[slot].pattern = dup;
    }
    if (ud->callbacks[slot].lua_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->callbacks[slot].lua_ref);
    }
    lua_pushvalue(L, 3);
    ud->callbacks[slot].lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_pushboolean(L, 1);
    return 1;
}

/* off([topic]) -> true. Removes the callback registered for an exact pattern,
 * or all callbacks when no topic is given. */
static int lua_module_mqtt_off(lua_State *L)
{
    lua_module_mqtt_ud_t *ud = lua_module_mqtt_get_ud(L, 1);
    const char *topic = luaL_optstring(L, 2, NULL);

    if (!topic) {
        lua_module_mqtt_free_callbacks(L, ud);
        lua_pushboolean(L, 1);
        return 1;
    }

    for (int i = 0; i < LUA_MODULE_MQTT_MAX_CALLBACKS; i++) {
        if (ud->callbacks[i].pattern && strcmp(ud->callbacks[i].pattern, topic) == 0) {
            if (ud->callbacks[i].lua_ref != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, ud->callbacks[i].lua_ref);
                ud->callbacks[i].lua_ref = LUA_NOREF;
            }
            free(ud->callbacks[i].pattern);
            ud->callbacks[i].pattern = NULL;
            break;
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* dispatch() -> count. Drains the rx queue and invokes every registered callback
 * whose pattern matches each message's topic. One message may fire several
 * callbacks. Returns the number of callback invocations. */
static int lua_module_mqtt_dispatch(lua_State *L)
{
    lua_module_mqtt_ud_t *ud = lua_module_mqtt_get_ud(L, 1);
    int count = 0;

    lua_module_mqtt_rx_msg_t *msg = NULL;
    while (xQueueReceive(ud->rx_queue, &msg, 0) == pdTRUE) {
        for (int i = 0; i < LUA_MODULE_MQTT_MAX_CALLBACKS; i++) {
            if (ud->callbacks[i].lua_ref == LUA_NOREF || !ud->callbacks[i].pattern) {
                continue;
            }
            if (!lua_module_mqtt_topic_matches(ud->callbacks[i].pattern, msg->topic)) {
                continue;
            }

            lua_rawgeti(L, LUA_REGISTRYINDEX, ud->callbacks[i].lua_ref);
            lua_newtable(L);
            lua_pushstring(L, msg->topic);
            lua_setfield(L, -2, "topic");
            lua_pushlstring(L, msg->payload, msg->payload_len);
            lua_setfield(L, -2, "payload");

            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                ESP_LOGW(LUA_MODULE_MQTT_TAG, "mqtt callback error: %s", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            count++;
        }
        lua_module_mqtt_rx_msg_free(msg);
    }

    lua_pushinteger(L, count);
    return 1;
}

static int lua_module_mqtt_disconnect(lua_State *L)
{
    lua_module_mqtt_ud_t *ud = lua_module_mqtt_get_ud(L, 1);
    if (ud->started) {
        esp_mqtt_client_stop(ud->client);
        ud->started = false;
        xEventGroupClearBits(ud->state, LUA_MODULE_MQTT_CONNECTED_BIT);
    }
    return 0;
}

/* new([uri[, opts]]) -> client. opts: username, password, client_id, keepalive,
 * rx_queue_len. uri example: "mqtt://192.168.1.10:1883". When uri or a
 * credential field is omitted, the value configured via the web UI (injected
 * with lua_module_mqtt_set_defaults) is used. */
static int lua_module_mqtt_new(lua_State *L)
{
    const char *uri = luaL_optstring(L, 1, s_default_uri);
    int rx_queue_len = LUA_MODULE_MQTT_DEFAULT_QLEN;

    if (!uri || !uri[0]) {
        return luaL_error(L, "mqtt new: uri required (none given and no broker configured)");
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = s_default_username,
        .credentials.authentication.password = s_default_password,
        .credentials.client_id = s_default_client_id,
    };

    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "username");
        if (lua_isstring(L, -1)) {
            cfg.credentials.username = lua_tostring(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 2, "password");
        if (lua_isstring(L, -1)) {
            cfg.credentials.authentication.password = lua_tostring(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 2, "client_id");
        if (lua_isstring(L, -1)) {
            cfg.credentials.client_id = lua_tostring(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 2, "keepalive");
        if (lua_isinteger(L, -1)) {
            cfg.session.keepalive = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 2, "rx_queue_len");
        if (lua_isinteger(L, -1)) {
            rx_queue_len = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    }

    if (rx_queue_len <= 0 || rx_queue_len > LUA_MODULE_MQTT_MAX_QLEN) {
        return luaL_error(L, "mqtt rx_queue_len must be in range 1-%d", LUA_MODULE_MQTT_MAX_QLEN);
    }

    lua_module_mqtt_ud_t *ud =
        (lua_module_mqtt_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    for (int i = 0; i < LUA_MODULE_MQTT_MAX_CALLBACKS; i++) {
        ud->callbacks[i].lua_ref = LUA_NOREF;
    }
    luaL_getmetatable(L, LUA_MODULE_MQTT_METATABLE);
    lua_setmetatable(L, -2);

    ud->rx_queue = xQueueCreate(rx_queue_len, sizeof(lua_module_mqtt_rx_msg_t *));
    ud->state = xEventGroupCreate();
    if (!ud->rx_queue || !ud->state) {
        lua_module_mqtt_destroy(ud);
        return luaL_error(L, "mqtt new: out of memory");
    }

    ud->client = esp_mqtt_client_init(&cfg);
    if (!ud->client) {
        lua_module_mqtt_destroy(ud);
        return luaL_error(L, "mqtt new: client init failed");
    }

    esp_err_t err = esp_mqtt_client_register_event(ud->client, ESP_EVENT_ANY_ID,
                                                   lua_module_mqtt_event_handler, ud);
    if (err != ESP_OK) {
        lua_module_mqtt_destroy(ud);
        return luaL_error(L, "mqtt new: register event failed: %s", esp_err_to_name(err));
    }

    return 1;
}

int luaopen_mqtt(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_MQTT_METATABLE)) {
        lua_pushcfunction(L, lua_module_mqtt_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_module_mqtt_connect);
        lua_setfield(L, -2, "connect");
        lua_pushcfunction(L, lua_module_mqtt_disconnect);
        lua_setfield(L, -2, "disconnect");
        lua_pushcfunction(L, lua_module_mqtt_is_connected);
        lua_setfield(L, -2, "is_connected");
        lua_pushcfunction(L, lua_module_mqtt_publish);
        lua_setfield(L, -2, "publish");
        lua_pushcfunction(L, lua_module_mqtt_subscribe);
        lua_setfield(L, -2, "subscribe");
        lua_pushcfunction(L, lua_module_mqtt_unsubscribe);
        lua_setfield(L, -2, "unsubscribe");
        lua_pushcfunction(L, lua_module_mqtt_poll);
        lua_setfield(L, -2, "poll");
        lua_pushcfunction(L, lua_module_mqtt_on);
        lua_setfield(L, -2, "on");
        lua_pushcfunction(L, lua_module_mqtt_off);
        lua_setfield(L, -2, "off");
        lua_pushcfunction(L, lua_module_mqtt_dispatch);
        lua_setfield(L, -2, "dispatch");
        lua_pushcfunction(L, lua_module_mqtt_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_module_mqtt_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_mqtt_register(void)
{
    return cap_lua_register_module("mqtt", luaopen_mqtt);
}
