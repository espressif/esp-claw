/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_knob.h"

#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "iot_knob.h"
#include "cap_lua.h"

static const char *TAG = "lua_knob";

#define KNOB_MAX_HANDLES      4
#define KNOB_EVENT_QUEUE_SIZE 32
#define KNOB_HANDLE_METATABLE "lua_knob_handle"

typedef struct {
    knob_handle_t handle;
    knob_event_t  event;
    int           count_value;
} knob_event_item_t;

typedef struct {
    knob_handle_t handle;
    int           lua_refs[KNOB_EVENT_MAX];
    bool          cbs_registered[KNOB_EVENT_MAX];
} knob_lua_reg_t;

typedef struct {
    knob_handle_t handle;
} knob_lua_handle_ud_t;

static knob_lua_reg_t s_regs[KNOB_MAX_HANDLES];
static int            s_num_regs = 0;
static QueueHandle_t  s_queue    = NULL;

static const char *const s_event_names[KNOB_EVENT_MAX] = {
    "left",
    "right",
    "h_lim",
    "l_lim",
    "zero",
};

static const char *knob_event_to_str(knob_event_t ev)
{
    if ((unsigned)ev < KNOB_EVENT_MAX) {
        return s_event_names[ev];
    }
    return "unknown";
}

static knob_event_t knob_event_from_str(const char *s)
{
    for (int i = 0; i < KNOB_EVENT_MAX; i++) {
        if (strcmp(s, s_event_names[i]) == 0) {
            return (knob_event_t)i;
        }
    }
    return KNOB_EVENT_MAX;
}

static knob_lua_reg_t *knob_find_reg(knob_handle_t handle)
{
    for (int i = 0; i < s_num_regs; i++) {
        if (s_regs[i].handle == handle) {
            return &s_regs[i];
        }
    }
    return NULL;
}

static knob_lua_reg_t *knob_add_reg(knob_handle_t handle)
{
    knob_lua_reg_t *reg = knob_find_reg(handle);
    if (reg) {
        return reg;
    }
    if (s_num_regs >= KNOB_MAX_HANDLES) {
        return NULL;
    }

    reg = &s_regs[s_num_regs++];
    memset(reg, 0, sizeof(*reg));
    reg->handle = handle;
    for (int i = 0; i < KNOB_EVENT_MAX; i++) {
        reg->lua_refs[i] = LUA_NOREF;
    }
    return reg;
}

static void knob_remove_reg(lua_State *L, knob_lua_reg_t *reg)
{
    int idx = (int)(reg - s_regs);
    if (idx < 0 || idx >= s_num_regs) {
        return;
    }

    for (int i = 0; i < KNOB_EVENT_MAX; i++) {
        if (reg->cbs_registered[i]) {
            esp_err_t err = iot_knob_unregister_cb(reg->handle, (knob_event_t)i);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "unregister_cb failed for event %s: %s",
                         knob_event_to_str((knob_event_t)i), esp_err_to_name(err));
            }
        }
        if (reg->lua_refs[i] != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, reg->lua_refs[i]);
        }
    }

    s_num_regs--;
    if (idx != s_num_regs) {
        s_regs[idx] = s_regs[s_num_regs];
    }
    memset(&s_regs[s_num_regs], 0, sizeof(s_regs[s_num_regs]));
}

static knob_lua_handle_ud_t *lua_knob_check_ud(lua_State *L, int idx)
{
    knob_lua_handle_ud_t *ud =
        (knob_lua_handle_ud_t *)luaL_checkudata(L, idx, KNOB_HANDLE_METATABLE);

    if (!ud || !ud->handle) {
        luaL_error(L, "knob handle expected");
        return NULL;
    }
    return ud;
}

static knob_handle_t lua_knob_check_handle(lua_State *L, int idx)
{
    return lua_knob_check_ud(L, idx)->handle;
}

static int lua_knob_handle_gc(lua_State *L)
{
    knob_lua_handle_ud_t *ud =
        (knob_lua_handle_ud_t *)luaL_testudata(L, 1, KNOB_HANDLE_METATABLE);
    knob_lua_reg_t *reg = NULL;

    if (!ud || !ud->handle) {
        return 0;
    }

    reg = knob_find_reg(ud->handle);
    if (reg) {
        knob_remove_reg(L, reg);
    }
    iot_knob_delete(ud->handle);
    ud->handle = NULL;
    return 0;
}

static void knob_c_event_cb(void *knob_handle, void *user_data)
{
    (void)user_data;

    if (!s_queue) {
        return;
    }

    knob_handle_t handle = (knob_handle_t)knob_handle;
    knob_event_item_t item = {
        .handle      = handle,
        .event       = iot_knob_get_event(handle),
        .count_value = iot_knob_get_count_value(handle),
    };

    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped knob event");
    }
}

/* knob.new(gpio_a, gpio_b [, default_direction]) -> handle | nil, errmsg */
static int lua_knob_new(lua_State *L)
{
    int gpio_a    = (int)luaL_checkinteger(L, 1);
    int gpio_b    = (int)luaL_checkinteger(L, 2);
    int direction = (int)luaL_optinteger(L, 3, 0);

    knob_config_t cfg = {
        .default_direction = (uint8_t)direction,
        .gpio_encoder_a    = (uint8_t)gpio_a,
        .gpio_encoder_b    = (uint8_t)gpio_b,
    };

    knob_handle_t knob = iot_knob_create(&cfg);
    if (knob == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "knob.new failed for gpio_a=%d gpio_b=%d", gpio_a, gpio_b);
        return 2;
    }

    if (!knob_add_reg(knob)) {
        iot_knob_delete(knob);
        lua_pushnil(L);
        lua_pushfstring(L, "too many knob handles (max %d)", KNOB_MAX_HANDLES);
        return 2;
    }

    knob_lua_handle_ud_t *ud =
        (knob_lua_handle_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->handle = knob;
    luaL_getmetatable(L, KNOB_HANDLE_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

/* knob.close(handle) -> true | nil, errmsg */
static int lua_knob_close(lua_State *L)
{
    knob_lua_handle_ud_t *ud = lua_knob_check_ud(L, 1);
    knob_handle_t handle = ud->handle;
    knob_lua_reg_t *reg  = knob_find_reg(handle);

    if (!reg) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid or closed knob handle");
        return 2;
    }

    knob_remove_reg(L, reg);

    esp_err_t err = iot_knob_delete(handle);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "knob.close failed: %s", esp_err_to_name(err));
        return 2;
    }

    ud->handle = NULL;
    lua_pushboolean(L, 1);
    return 1;
}

/* knob.get_count(handle) -> count */
static int lua_knob_get_count(lua_State *L)
{
    knob_handle_t handle = lua_knob_check_handle(L, 1);
    lua_pushinteger(L, iot_knob_get_count_value(handle));
    return 1;
}

/* knob.clear_count(handle) -> true | nil, errmsg */
static int lua_knob_clear_count(lua_State *L)
{
    knob_handle_t handle = lua_knob_check_handle(L, 1);
    esp_err_t err = iot_knob_clear_count_value(handle);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "knob.clear_count failed: %s", esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* knob.on(handle, event_str, fn) -> true | nil, errmsg */
static int lua_knob_on(lua_State *L)
{
    knob_handle_t  handle    = lua_knob_check_handle(L, 1);
    const char    *event_str = luaL_checkstring(L, 2);
    knob_lua_reg_t *reg      = knob_find_reg(handle);

    luaL_checktype(L, 3, LUA_TFUNCTION);

    if (!reg) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid or closed knob handle");
        return 2;
    }

    knob_event_t ev = knob_event_from_str(event_str);
    if (ev == KNOB_EVENT_MAX) {
        lua_pushnil(L);
        lua_pushfstring(L, "unknown event '%s'", event_str);
        return 2;
    }

    if (!s_queue) {
        s_queue = xQueueCreate(KNOB_EVENT_QUEUE_SIZE, sizeof(knob_event_item_t));
        if (!s_queue) {
            lua_pushnil(L);
            lua_pushstring(L, "failed to create event queue");
            return 2;
        }
    }

    if (reg->lua_refs[ev] != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, reg->lua_refs[ev]);
    }
    lua_pushvalue(L, 3);
    reg->lua_refs[ev] = luaL_ref(L, LUA_REGISTRYINDEX);

    if (!reg->cbs_registered[ev]) {
        esp_err_t err = iot_knob_register_cb(handle, ev, knob_c_event_cb, NULL);
        if (err != ESP_OK) {
            luaL_unref(L, LUA_REGISTRYINDEX, reg->lua_refs[ev]);
            reg->lua_refs[ev] = LUA_NOREF;
            lua_pushnil(L);
            lua_pushfstring(L, "register_cb failed for event '%s': %s",
                            event_str, esp_err_to_name(err));
            return 2;
        }
        reg->cbs_registered[ev] = true;
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* knob.off(handle [, event_str]) -> true | nil, errmsg */
static int lua_knob_off(lua_State *L)
{
    knob_handle_t   handle    = lua_knob_check_handle(L, 1);
    const char     *event_str = luaL_optstring(L, 2, NULL);
    knob_lua_reg_t *reg       = knob_find_reg(handle);

    if (!reg) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid or closed knob handle");
        return 2;
    }

    if (event_str) {
        knob_event_t ev = knob_event_from_str(event_str);
        if (ev == KNOB_EVENT_MAX) {
            lua_pushnil(L);
            lua_pushfstring(L, "unknown event '%s'", event_str);
            return 2;
        }
        if (reg->lua_refs[ev] != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, reg->lua_refs[ev]);
            reg->lua_refs[ev] = LUA_NOREF;
        }
        if (reg->cbs_registered[ev]) {
            esp_err_t err = iot_knob_unregister_cb(handle, ev);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                lua_pushnil(L);
                lua_pushfstring(L, "unregister_cb failed for event '%s': %s",
                                event_str, esp_err_to_name(err));
                return 2;
            }
            reg->cbs_registered[ev] = false;
        }
    } else {
        for (int i = 0; i < KNOB_EVENT_MAX; i++) {
            if (reg->lua_refs[i] != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, reg->lua_refs[i]);
                reg->lua_refs[i] = LUA_NOREF;
            }
            if (reg->cbs_registered[i]) {
                esp_err_t err = iot_knob_unregister_cb(handle, (knob_event_t)i);
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "unregister_cb failed for event %s: %s",
                             knob_event_to_str((knob_event_t)i), esp_err_to_name(err));
                }
                reg->cbs_registered[i] = false;
            }
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* knob.dispatch() -> count */
static int lua_knob_dispatch(lua_State *L)
{
    int count = 0;
    if (!s_queue) {
        lua_pushinteger(L, 0);
        return 1;
    }

    knob_event_item_t item;
    while (xQueueReceive(s_queue, &item, 0) == pdTRUE) {
        knob_lua_reg_t *reg = knob_find_reg(item.handle);
        if (!reg) {
            continue;
        }

        knob_event_t ev = item.event;
        if ((unsigned)ev >= KNOB_EVENT_MAX) {
            continue;
        }

        int ref = reg->lua_refs[ev];
        if (ref == LUA_NOREF) {
            continue;
        }

        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_newtable(L);
        lua_pushstring(L, knob_event_to_str(ev));
        lua_setfield(L, -2, "event");
        lua_pushinteger(L, item.count_value);
        lua_setfield(L, -2, "count");

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            ESP_LOGW(TAG, "knob callback error: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        count++;
    }

    lua_pushinteger(L, count);
    return 1;
}

int luaopen_knob(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"new",         lua_knob_new},
        {"close",       lua_knob_close},
        {"get_count",   lua_knob_get_count},
        {"clear_count", lua_knob_clear_count},
        {"on",          lua_knob_on},
        {"off",         lua_knob_off},
        {"dispatch",    lua_knob_dispatch},
        {NULL, NULL},
    };
    if (luaL_newmetatable(L, KNOB_HANDLE_METATABLE)) {
        lua_pushcfunction(L, lua_knob_handle_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t lua_module_knob_register(void)
{
    return cap_lua_register_module("knob", luaopen_knob);
}
