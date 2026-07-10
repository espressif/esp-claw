/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_button.h"

#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "cap_lua.h"

static const char *TAG = "lua_button";

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define BTN_MAX_HANDLES      8
#define BTN_EVENT_QUEUE_SIZE 16
#define BTN_HANDLE_METATABLE "lua_button_handle"

/* --------------------------------------------------------------------------
 * Internal types
 * -------------------------------------------------------------------------- */
typedef struct {
    button_handle_t handle;
    button_event_t  event;
    uint8_t         repeat_count;
    uint32_t        pressed_time_ms;
} btn_event_item_t;

typedef struct {
    button_handle_t handle;
    int             lua_refs[BUTTON_EVENT_MAX]; /* LUA_NOREF if not registered */
    bool            cbs_registered[BUTTON_EVENT_MAX];
} btn_lua_reg_t;

typedef struct {
    button_handle_t handle;
} btn_lua_handle_ud_t;

/* --------------------------------------------------------------------------
 * Module-level state
 * -------------------------------------------------------------------------- */
static btn_lua_reg_t s_regs[BTN_MAX_HANDLES];
static int           s_num_regs = 0;
static QueueHandle_t s_queue    = NULL;

/* --------------------------------------------------------------------------
 * Event name tables
 * -------------------------------------------------------------------------- */
static const char *const s_event_names[BUTTON_EVENT_MAX] = {
    "press_down",
    "press_up",
    "press_repeat",
    "press_repeat_done",
    "single_click",
    "double_click",
    "multiple_click",
    "long_press_start",
    "long_press_hold",
    "long_press_up",
    "press_end",
};

static const char *btn_event_to_str(button_event_t ev)
{
    if ((unsigned)ev < BUTTON_EVENT_MAX) {
        return s_event_names[ev];
    }
    return "unknown";
}

static button_event_t btn_event_from_str(const char *s)
{
    for (int i = 0; i < BUTTON_EVENT_MAX; i++) {
        if (strcmp(s, s_event_names[i]) == 0) {
            return (button_event_t)i;
        }
    }
    return BUTTON_EVENT_MAX;
}

/* --------------------------------------------------------------------------
 * Registry helpers
 * -------------------------------------------------------------------------- */
static btn_lua_reg_t *btn_find_reg(button_handle_t handle)
{
    for (int i = 0; i < s_num_regs; i++) {
        if (s_regs[i].handle == handle) {
            return &s_regs[i];
        }
    }
    return NULL;
}

static btn_lua_reg_t *btn_add_reg(button_handle_t handle)
{
    btn_lua_reg_t *reg = btn_find_reg(handle);
    if (reg) {
        return reg;
    }
    if (s_num_regs >= BTN_MAX_HANDLES) {
        return NULL;
    }

    reg = &s_regs[s_num_regs++];
    memset(reg, 0, sizeof(*reg));
    reg->handle = handle;
    for (int i = 0; i < BUTTON_EVENT_MAX; i++) {
        reg->lua_refs[i] = LUA_NOREF;
    }
    return reg;
}

static void btn_remove_reg(lua_State *L, btn_lua_reg_t *reg)
{
    int idx = (int)(reg - s_regs);
    if (idx < 0 || idx >= s_num_regs) {
        return;
    }

    for (int i = 0; i < BUTTON_EVENT_MAX; i++) {
        if (reg->cbs_registered[i]) {
            esp_err_t err = iot_button_unregister_cb(reg->handle, (button_event_t)i, NULL);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "unregister_cb failed for event %s: %s",
                         btn_event_to_str((button_event_t)i), esp_err_to_name(err));
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

static btn_lua_handle_ud_t *lua_btn_check_ud(lua_State *L, int idx)
{
    btn_lua_handle_ud_t *ud =
        (btn_lua_handle_ud_t *)luaL_checkudata(L, idx, BTN_HANDLE_METATABLE);

    if (!ud || !ud->handle) {
        luaL_error(L, "button handle expected");
        return NULL;
    }
    return ud;
}

static button_handle_t lua_btn_check_handle(lua_State *L, int idx)
{
    return lua_btn_check_ud(L, idx)->handle;
}

static int lua_btn_handle_gc(lua_State *L)
{
    btn_lua_handle_ud_t *ud =
        (btn_lua_handle_ud_t *)luaL_testudata(L, 1, BTN_HANDLE_METATABLE);
    btn_lua_reg_t *reg = NULL;

    if (!ud || !ud->handle) {
        return 0;
    }

    reg = btn_find_reg(ud->handle);
    if (reg) {
        btn_remove_reg(L, reg);
    }
    iot_button_delete(ud->handle);
    ud->handle = NULL;
    return 0;
}

static void btn_c_event_cb(void *btn_handle, void *user_data)
{
    (void)user_data;

    if (!s_queue) {
        return;
    }

    button_handle_t handle = (button_handle_t)btn_handle;
    btn_event_item_t item = {
        .handle          = handle,
        .event           = iot_button_get_event(handle),
        .repeat_count    = iot_button_get_repeat(handle),
        .pressed_time_ms = iot_button_get_pressed_time(handle),
    };

    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped button event");
    }
}

/* --------------------------------------------------------------------------
 * button.new(gpio_num [, active_level [, long_press_ms [, short_press_ms]]])
 *   -> handle | nil, errmsg
 * -------------------------------------------------------------------------- */
static int lua_btn_new(lua_State *L)
{
    int gpio_num        = (int)luaL_checkinteger(L, 1);
    int active_level    = (int)luaL_optinteger(L, 2, 0);
    int long_press_ms   = (int)luaL_optinteger(L, 3, 0);
    int short_press_ms  = (int)luaL_optinteger(L, 4, 0);
    button_handle_t btn = NULL;

    if (active_level != 0 && active_level != 1) {
        lua_pushnil(L);
        lua_pushstring(L, "active_level must be 0 or 1");
        return 2;
    }

    button_config_t btn_cfg = {
        .long_press_time  = (uint16_t)long_press_ms,
        .short_press_time = (uint16_t)short_press_ms,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num          = gpio_num,
        .active_level      = (uint8_t)active_level,
        .enable_power_save = false,
        .disable_pull      = false,
    };

    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn);
    if (err != ESP_OK || btn == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "button.new failed for gpio %d: %s",
                        gpio_num, esp_err_to_name(err));
        return 2;
    }

    if (!btn_add_reg(btn)) {
        iot_button_delete(btn);
        lua_pushnil(L);
        lua_pushfstring(L, "too many button handles (max %d)", BTN_MAX_HANDLES);
        return 2;
    }

    btn_lua_handle_ud_t *ud =
        (btn_lua_handle_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->handle = btn;
    luaL_getmetatable(L, BTN_HANDLE_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

/* --------------------------------------------------------------------------
 * button.close(handle) -> true | nil, errmsg
 * -------------------------------------------------------------------------- */
static int lua_btn_close(lua_State *L)
{
    btn_lua_handle_ud_t *ud = lua_btn_check_ud(L, 1);
    button_handle_t handle = ud->handle;
    btn_lua_reg_t  *reg    = btn_find_reg(handle);
    esp_err_t       err;

    if (!reg) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid or closed button handle");
        return 2;
    }

    btn_remove_reg(L, reg);

    err = iot_button_delete(handle);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "button.close failed: %s", esp_err_to_name(err));
        return 2;
    }

    ud->handle = NULL;
    lua_pushboolean(L, 1);
    return 1;
}

/* --------------------------------------------------------------------------
 * button.get_key_level(handle) -> level
 * -------------------------------------------------------------------------- */
static int lua_btn_get_key_level(lua_State *L)
{
    button_handle_t handle = lua_btn_check_handle(L, 1);
    btn_lua_reg_t  *reg    = btn_find_reg(handle);

    if (!reg) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid or closed button handle");
        return 2;
    }

    lua_pushinteger(L, iot_button_get_key_level(handle));
    return 1;
}

/* --------------------------------------------------------------------------
 * button.on(handle, event_str, fn) -> true | nil, errmsg
 * -------------------------------------------------------------------------- */
static int lua_btn_on(lua_State *L)
{
    button_handle_t handle    = lua_btn_check_handle(L, 1);
    const char     *event_str = luaL_checkstring(L, 2);
    btn_lua_reg_t  *reg       = btn_find_reg(handle);

    luaL_checktype(L, 3, LUA_TFUNCTION);

    if (!reg) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid or closed button handle");
        return 2;
    }

    button_event_t ev = btn_event_from_str(event_str);
    if (ev == BUTTON_EVENT_MAX) {
        lua_pushnil(L);
        lua_pushfstring(L, "unknown event '%s'", event_str);
        return 2;
    }

    if (!s_queue) {
        s_queue = xQueueCreate(BTN_EVENT_QUEUE_SIZE, sizeof(btn_event_item_t));
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
        esp_err_t err = iot_button_register_cb(handle, ev, NULL, btn_c_event_cb, NULL);
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

/* --------------------------------------------------------------------------
 * button.off(handle [, event_str]) -> true | nil, errmsg
 * -------------------------------------------------------------------------- */
static int lua_btn_off(lua_State *L)
{
    button_handle_t handle    = lua_btn_check_handle(L, 1);
    const char     *event_str = luaL_optstring(L, 2, NULL);
    btn_lua_reg_t  *reg       = btn_find_reg(handle);

    if (!reg) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid or closed button handle");
        return 2;
    }

    if (event_str) {
        button_event_t ev = btn_event_from_str(event_str);
        if (ev == BUTTON_EVENT_MAX) {
            lua_pushnil(L);
            lua_pushfstring(L, "unknown event '%s'", event_str);
            return 2;
        }
        if (reg->lua_refs[ev] != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, reg->lua_refs[ev]);
            reg->lua_refs[ev] = LUA_NOREF;
        }
        if (reg->cbs_registered[ev]) {
            esp_err_t err = iot_button_unregister_cb(handle, ev, NULL);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                lua_pushnil(L);
                lua_pushfstring(L, "unregister_cb failed for event '%s': %s",
                                event_str, esp_err_to_name(err));
                return 2;
            }
            reg->cbs_registered[ev] = false;
        }
    } else {
        for (int i = 0; i < BUTTON_EVENT_MAX; i++) {
            if (reg->lua_refs[i] != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, reg->lua_refs[i]);
                reg->lua_refs[i] = LUA_NOREF;
            }
            if (reg->cbs_registered[i]) {
                esp_err_t err = iot_button_unregister_cb(handle, (button_event_t)i, NULL);
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                    lua_pushnil(L);
                    lua_pushfstring(L, "unregister_cb failed for event '%s': %s",
                                    btn_event_to_str((button_event_t)i), esp_err_to_name(err));
                    return 2;
                }
                reg->cbs_registered[i] = false;
            }
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* --------------------------------------------------------------------------
 * button.dispatch() -> count
 * -------------------------------------------------------------------------- */
static int lua_btn_dispatch(lua_State *L)
{
    int count = 0;
    if (!s_queue) {
        lua_pushinteger(L, 0);
        return 1;
    }

    btn_event_item_t item;
    while (xQueueReceive(s_queue, &item, 0) == pdTRUE) {
        btn_lua_reg_t *reg = btn_find_reg(item.handle);
        if (!reg) {
            continue;
        }

        button_event_t ev = item.event;
        if ((unsigned)ev >= BUTTON_EVENT_MAX) {
            continue;
        }

        int ref = reg->lua_refs[ev];
        if (ref == LUA_NOREF) {
            continue;
        }

        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_newtable(L);
        lua_pushlightuserdata(L, item.handle);
        lua_setfield(L, -2, "handle");
        lua_pushstring(L, btn_event_to_str(ev));
        lua_setfield(L, -2, "event");
        lua_pushinteger(L, item.repeat_count);
        lua_setfield(L, -2, "repeat_count");
        lua_pushinteger(L, item.pressed_time_ms);
        lua_setfield(L, -2, "pressed_time_ms");

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            ESP_LOGW(TAG, "button callback error: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        count++;
    }

    lua_pushinteger(L, count);
    return 1;
}

/* --------------------------------------------------------------------------
 * Module registration
 * -------------------------------------------------------------------------- */
int luaopen_button(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"new",           lua_btn_new},
        {"close",         lua_btn_close},
        {"get_key_level", lua_btn_get_key_level},
        {"on",            lua_btn_on},
        {"off",           lua_btn_off},
        {"dispatch",      lua_btn_dispatch},
        {NULL, NULL},
    };
    if (luaL_newmetatable(L, BTN_HANDLE_METATABLE)) {
        lua_pushcfunction(L, lua_btn_handle_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t lua_module_button_register(void)
{
    return cap_lua_register_module("button", luaopen_button);
}
