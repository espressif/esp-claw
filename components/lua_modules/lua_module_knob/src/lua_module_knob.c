/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPDX-License-Identifier: Apache-2.0 */
#include "lua_module_knob.h"

#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "iot_knob.h"
#include "cap_lua.h"
#include "cap_lua_hw.h"

#define KNOB_MAX_HANDLES 4
#define KNOB_EVENT_QUEUE_SIZE 32
#define KNOB_HANDLE_METATABLE "lua_knob_handle"

static const char *TAG = "lua_knob";
static const char s_knobs_key;
static const char *const s_event_names[KNOB_EVENT_MAX] = {"left", "right", "h_lim", "l_lim", "zero"};

typedef struct {
    knob_event_t event;
    int count;
} knob_event_item_t;

typedef struct {
    knob_handle_t handle;
    QueueHandle_t queue;
    cap_lua_hw_lease_t lease;
    lua_State *state;
    int refs[KNOB_EVENT_MAX];
    knob_config_t config;
} knob_lua_handle_t;

static int push_knobs(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &s_knobs_key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_newtable(L);
        lua_pushliteral(L, "k");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &s_knobs_key);
    }
    return lua_gettop(L);
}

static knob_event_t event_from_str(const char *name)
{
    for (int i = 0; i < KNOB_EVENT_MAX; ++i) {
        if (!strcmp(name, s_event_names[i])) return i;
    }
    return KNOB_EVENT_MAX;
}

static knob_lua_handle_t *check_knob(lua_State *L)
{
    knob_lua_handle_t *ud = luaL_checkudata(L, 1, KNOB_HANDLE_METATABLE);
    if (!ud->handle) luaL_error(L, "knob handle is closed");
    return ud;
}

static void knob_event_cb(void *handle, void *arg)
{
    knob_lua_handle_t *ud = arg;
    const knob_event_item_t event = {.event = iot_knob_get_event(handle), .count = iot_knob_get_count_value(handle)};
    if (xQueueSend(ud->queue, &event, 0) != pdTRUE) ESP_LOGW(TAG, "Event queue full");
}

/* Vendor global lists and callbacks are accessed only from ESP_TIMER_TASK. */
static esp_err_t create_knob(void *arg)
{
    knob_lua_handle_t *ud = arg;
    ud->handle = iot_knob_create(&ud->config);
    if (!ud->handle) return ESP_FAIL;
    for (int i = 0; i < KNOB_EVENT_MAX; ++i) {
        esp_err_t err = iot_knob_register_cb(ud->handle, i, knob_event_cb, ud);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t delete_knob(void *arg)
{
    knob_lua_handle_t *ud = arg;
    esp_err_t err = iot_knob_delete(ud->handle);
    if (err == ESP_OK) ud->handle = NULL;
    return err;
}

static esp_err_t close_knob(knob_lua_handle_t *ud)
{
    if (ud->handle) {
        esp_err_t err = cap_lua_hw_timer_call(delete_knob, ud);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Knob close failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (ud->queue) {
        vQueueDelete(ud->queue);
        ud->queue = NULL;
    }
    for (int i = 0; i < KNOB_EVENT_MAX; ++i) {
        if (ud->refs[i] != LUA_NOREF) {
            luaL_unref(ud->state, LUA_REGISTRYINDEX, ud->refs[i]);
            ud->refs[i] = LUA_NOREF;
        }
    }
    return cap_lua_hw_release(&ud->lease);
}

static int lua_knob_gc(lua_State *L)
{
    knob_lua_handle_t *ud = luaL_testudata(L, 1, KNOB_HANDLE_METATABLE);
    if (ud) (void)close_knob(ud);
    return 0;
}

static int lua_knob_new(lua_State *L)
{
    lua_Integer gpio_a = luaL_checkinteger(L, 1);
    lua_Integer gpio_b = luaL_checkinteger(L, 2);
    int direction = luaL_optinteger(L, 3, 0);
    if (gpio_a < 0 || gpio_a >= GPIO_NUM_MAX || gpio_b < 0 || gpio_b >= GPIO_NUM_MAX ||
        !GPIO_IS_VALID_GPIO(gpio_a) || !GPIO_IS_VALID_GPIO(gpio_b) || gpio_a == gpio_b || (direction != 0 && direction != 1)) {
        return luaL_error(L, "knob.new: invalid GPIO or direction");
    }
    int table = push_knobs(L);
    unsigned count = 0;
    lua_pushnil(L);
    while (lua_next(L, table)) {
        ++count;
        lua_pop(L, 1);
    }
    if (count >= KNOB_MAX_HANDLES) return luaL_error(L, "too many knob handles");
    knob_lua_handle_t *ud = lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    for (int i = 0; i < KNOB_EVENT_MAX; ++i) ud->refs[i] = LUA_NOREF;
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    ud->state = lua_tothread(L, -1);
    lua_pop(L, 1);
    ud->config = (knob_config_t){.gpio_encoder_a = gpio_a, .gpio_encoder_b = gpio_b, .default_direction = direction};
    luaL_setmetatable(L, KNOB_HANDLE_METATABLE);
    char key_a[CLAW_HW_KEY_SIZE], key_b[CLAW_HW_KEY_SIZE];
    const char *keys[] = {claw_hw_key_gpio(key_a, sizeof(key_a), gpio_a), claw_hw_key_gpio(key_b, sizeof(key_b), gpio_b)};
    const char *owner = cap_lua_current_owner_tag();
    esp_err_t err = cap_lua_hw_claim(L, &ud->lease, owner ? owner : "lua/knob", keys, 2);
    if (err == ESP_OK) {
        ud->queue = xQueueCreate(KNOB_EVENT_QUEUE_SIZE, sizeof(knob_event_item_t));
        err = ud->queue ? cap_lua_hw_timer_call(create_knob, ud) : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        (void)close_knob(ud);
        lua_pushnil(L);
        lua_pushfstring(L, "knob.new failed: %s", esp_err_to_name(err));
        return 2;
    }
    lua_pushvalue(L, -1);
    lua_pushboolean(L, 1);
    lua_rawset(L, table);
    return 1;
}

static int lua_knob_close(lua_State *L)
{
    knob_lua_handle_t *ud = luaL_checkudata(L, 1, KNOB_HANDLE_METATABLE);
    esp_err_t err = close_knob(ud);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "knob.close failed: %s", esp_err_to_name(err));
        return 2;
    }
    push_knobs(L);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

typedef struct {
    knob_handle_t handle;
    int value;
    bool clear;
} knob_count_request_t;

static esp_err_t knob_count_op(void *arg)
{
    knob_count_request_t *request = arg;
    if (request->clear) return iot_knob_clear_count_value(request->handle);
    request->value = iot_knob_get_count_value(request->handle);
    return ESP_OK;
}

static int lua_knob_get_count(lua_State *L)
{
    knob_count_request_t request = {.handle = check_knob(L)->handle};
    esp_err_t err = cap_lua_hw_timer_call(knob_count_op, &request);
    if (err != ESP_OK) return luaL_error(L, "knob.get_count failed: %s", esp_err_to_name(err));
    lua_pushinteger(L, request.value);
    return 1;
}

static int lua_knob_clear_count(lua_State *L)
{
    knob_count_request_t request = {.handle = check_knob(L)->handle, .clear = true};
    esp_err_t err = cap_lua_hw_timer_call(knob_count_op, &request);
    if (err != ESP_OK) return luaL_error(L, "knob.clear_count failed: %s", esp_err_to_name(err));
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_knob_on(lua_State *L)
{
    knob_lua_handle_t *ud = check_knob(L);
    knob_event_t event = event_from_str(luaL_checkstring(L, 2));
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (event == KNOB_EVENT_MAX) return luaL_error(L, "unknown knob event");
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (ud->refs[event] != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ud->refs[event]);
    ud->refs[event] = ref;
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_knob_off(lua_State *L)
{
    knob_lua_handle_t *ud = check_knob(L);
    const char *name = luaL_optstring(L, 2, NULL);
    knob_event_t event = name ? event_from_str(name) : KNOB_EVENT_MAX;
    if (name && event == KNOB_EVENT_MAX) return luaL_error(L, "unknown knob event");
    for (int i = 0; i < KNOB_EVENT_MAX; ++i) {
        if ((!name || i == event) && ud->refs[i] != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ud->refs[i]);
            ud->refs[i] = LUA_NOREF;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_knob_dispatch(lua_State *L)
{
    int count = 0;
    int knobs = push_knobs(L);
    /* Callbacks may close or create knobs; iterate a stable Lua-owned snapshot. */
    lua_newtable(L);
    int snapshot = lua_gettop(L);
    int size = 0;
    lua_pushnil(L);
    while (lua_next(L, knobs)) {
        lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_rawseti(L, snapshot, ++size);
    }
    for (int i = 1; i <= size; ++i) {
        lua_rawgeti(L, snapshot, i);
        knob_lua_handle_t *ud = lua_touserdata(L, -1);
        knob_event_item_t event;
        while (ud->queue && xQueueReceive(ud->queue, &event, 0) == pdTRUE) {
            if ((unsigned)event.event >= KNOB_EVENT_MAX || ud->refs[event.event] == LUA_NOREF) continue;
            lua_rawgeti(L, LUA_REGISTRYINDEX, ud->refs[event.event]);
            lua_newtable(L);
            lua_pushstring(L, s_event_names[event.event]);
            lua_setfield(L, -2, "event");
            lua_pushinteger(L, event.count);
            lua_setfield(L, -2, "count");
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                ESP_LOGW(TAG, "Knob callback failed: %s", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            ++count;
        }
        lua_pop(L, 1);
    }
    lua_pushinteger(L, count);
    return 1;
}

int luaopen_knob(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"new", lua_knob_new}, {"close", lua_knob_close}, {"get_count", lua_knob_get_count},
        {"clear_count", lua_knob_clear_count}, {"on", lua_knob_on}, {"off", lua_knob_off},
        {"dispatch", lua_knob_dispatch}, {NULL, NULL},
    };
    if (luaL_newmetatable(L, KNOB_HANDLE_METATABLE)) {
        lua_pushcfunction(L, lua_knob_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    luaL_newlib(L, functions);
    return 1;
}

esp_err_t lua_module_knob_register(void)
{
    return cap_lua_register_module("knob", luaopen_knob);
}
