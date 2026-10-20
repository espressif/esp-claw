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
#include "cap_lua_hw.h"

static const char *TAG = "lua_button";

#define BTN_MAX_HANDLES      8
#define BTN_EVENT_QUEUE_SIZE 16
#define BTN_HANDLE_METATABLE "lua_button_handle"

typedef struct {
    button_handle_t handle;
    uint64_t        generation;
    button_event_t  event;
    uint8_t         repeat_count;
    uint32_t        pressed_time_ms;
} btn_event_item_t;

typedef struct {
    button_handle_t handle;
    uint64_t        generation;
    int             lua_refs[BUTTON_EVENT_MAX]; /* LUA_NOREF if not registered */
    bool            cbs_registered[BUTTON_EVENT_MAX];
} btn_lua_reg_t;

typedef struct {
    btn_lua_reg_t regs[BTN_MAX_HANDLES];
    int count;
    QueueHandle_t queue;
    uint64_t generation;
} btn_context_t;

typedef struct {
    button_handle_t handle;
    cap_lua_hw_lease_t lease;
    uint64_t generation;
    lua_State *state;
    btn_context_t *context;
} btn_lua_handle_ud_t;

static const char s_context_key;

static int btn_context_gc(lua_State *L)
{
    btn_context_t *ctx = lua_touserdata(L, 1);
    if (ctx && ctx->queue) {
        vQueueDelete(ctx->queue);
        ctx->queue = NULL;
    }
    return 0;
}

static btn_context_t *btn_context(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &s_context_key);
    btn_context_t *ctx = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

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

static btn_lua_reg_t *btn_find_reg(lua_State *L, button_handle_t handle)
{
    btn_context_t *ctx = btn_context(L);
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->regs[i].handle == handle) {
            return &ctx->regs[i];
        }
    }
    return NULL;
}

static btn_lua_reg_t *btn_add_reg(lua_State *L, button_handle_t handle)
{
    btn_context_t *ctx = btn_context(L);
    btn_lua_reg_t *reg = btn_find_reg(L, handle);
    if (reg) {
        return reg;
    }
    if (ctx->count >= BTN_MAX_HANDLES) {
        return NULL;
    }

    reg = &ctx->regs[ctx->count++];
    memset(reg, 0, sizeof(*reg));
    reg->handle = handle;
    for (int i = 0; i < BUTTON_EVENT_MAX; i++) {
        reg->lua_refs[i] = LUA_NOREF;
    }
    return reg;
}

static void btn_remove_reg(lua_State *L, btn_lua_reg_t *reg)
{
    btn_context_t *ctx = btn_context(L);
    int idx = (int)(reg - ctx->regs);
    if (idx < 0 || idx >= ctx->count) {
        return;
    }

    for (int i = 0; i < BUTTON_EVENT_MAX; i++) {
        if (reg->lua_refs[i] != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, reg->lua_refs[i]);
        }
    }

    ctx->count--;
    if (idx != ctx->count) {
        ctx->regs[idx] = ctx->regs[ctx->count];
    }
    memset(&ctx->regs[ctx->count], 0, sizeof(ctx->regs[ctx->count]));
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

typedef struct {
    btn_lua_handle_ud_t *object;
    const button_config_t *config;
    const button_gpio_config_t *gpio;
} btn_create_args_t;

static esp_err_t btn_create_driver(void *arg)
{
    btn_create_args_t *args = arg;
    return iot_button_new_gpio_device(args->config, args->gpio, &args->object->handle);
}

static esp_err_t btn_delete_driver(void *arg)
{
    btn_lua_handle_ud_t *ud = arg;
    return iot_button_delete(ud->handle);
}

static esp_err_t btn_close(btn_lua_handle_ud_t *ud)
{
    if (ud->handle) {
        btn_lua_reg_t *reg = btn_find_reg(ud->state, ud->handle);
        esp_err_t err = cap_lua_hw_timer_call(btn_delete_driver, ud);
        if (err != ESP_OK) return err;
        ud->handle = NULL;
        if (reg) btn_remove_reg(ud->state, reg);
    }
    return cap_lua_hw_release(&ud->lease);
}

static int lua_btn_handle_gc(lua_State *L)
{
    btn_lua_handle_ud_t *ud = luaL_testudata(L, 1, BTN_HANDLE_METATABLE);
    if (ud) (void)btn_close(ud);
    return 0;
}

static void btn_c_event_cb(void *btn_handle, void *user_data)
{
    btn_lua_handle_ud_t *ud = user_data;
    btn_context_t *ctx = ud->context;

    if (!ctx->queue) {
        return;
    }

    button_handle_t handle = (button_handle_t)btn_handle;
    btn_event_item_t item = {
        .handle          = handle,
        .generation      = ud->generation,
        .event           = iot_button_get_event(handle),
        .repeat_count    = iot_button_get_repeat(handle),
        .pressed_time_ms = iot_button_get_pressed_time(handle),
    };

    if (xQueueSend(ctx->queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped button event");
    }
}

typedef struct {
    button_handle_t handle;
    button_event_t event;
    btn_lua_handle_ud_t *object;
    bool enable;
    int level;
} btn_operation_t;

static esp_err_t btn_update_callback(void *arg)
{
    btn_operation_t *op = arg;
    return op->enable ? iot_button_register_cb(op->handle, op->event, NULL, btn_c_event_cb, op->object)
                      : iot_button_unregister_cb(op->handle, op->event, NULL);
}

static esp_err_t btn_read_level(void *arg)
{
    btn_operation_t *op = arg;
    op->level = iot_button_get_key_level(op->handle);
    return ESP_OK;
}

static esp_err_t btn_set_callback(button_handle_t handle, button_event_t event, btn_lua_handle_ud_t *object, bool enable)
{
    btn_operation_t op = { .handle = handle, .event = event, .object = object, .enable = enable };
    return cap_lua_hw_timer_call(btn_update_callback, &op);
}

static int lua_btn_new(lua_State *L)
{
    int gpio_num        = (int)luaL_checkinteger(L, 1);
    int active_level    = (int)luaL_optinteger(L, 2, 0);
    int long_press_ms   = (int)luaL_optinteger(L, 3, 0);
    int short_press_ms  = (int)luaL_optinteger(L, 4, 0);

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

    btn_lua_handle_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    *ud = (btn_lua_handle_ud_t){ .context = btn_context(L) };
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    ud->state = lua_tothread(L, -1);
    lua_pop(L, 1);
    if (ud->context->generation == UINT64_MAX) return luaL_error(L, "button generation exhausted");
    ud->generation = ++ud->context->generation;
    luaL_setmetatable(L, BTN_HANDLE_METATABLE);
    esp_err_t err = cap_lua_hw_claim_gpio(L, &ud->lease, "lua/button", gpio_num);
    if (err == ESP_OK) {
        btn_create_args_t create_args = { .object = ud, .config = &btn_cfg, .gpio = &gpio_cfg };
        err = cap_lua_hw_timer_call(btn_create_driver, &create_args);
    }
    if (err == ESP_OK) {
        btn_lua_reg_t *reg = btn_add_reg(L, ud->handle);
        if (reg) reg->generation = ud->generation;
        else err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK || !ud->handle) {
        (void)btn_close(ud);
        lua_pushnil(L);
        lua_pushfstring(L, "button.new failed for gpio %d: %s", gpio_num, esp_err_to_name(err));
        return 2;
    }
    return 1;
}

static int lua_btn_close(lua_State *L)
{
    btn_lua_handle_ud_t *ud = luaL_checkudata(L, 1, BTN_HANDLE_METATABLE);
    esp_err_t err = btn_close(ud);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "button.close failed: %s", esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_btn_get_key_level(lua_State *L)
{
    button_handle_t handle = lua_btn_check_handle(L, 1);
    btn_lua_reg_t  *reg    = btn_find_reg(L, handle);

    if (!reg) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid or closed button handle");
        return 2;
    }

    btn_operation_t op = { .handle = handle };
    esp_err_t err = cap_lua_hw_timer_call(btn_read_level, &op);
    if (err != ESP_OK) return luaL_error(L, "button read failed: %s", esp_err_to_name(err));
    lua_pushinteger(L, op.level);
    return 1;
}

static int lua_btn_on(lua_State *L)
{
    btn_context_t *ctx = btn_context(L);
    button_handle_t handle    = lua_btn_check_handle(L, 1);
    const char     *event_str = luaL_checkstring(L, 2);
    btn_lua_reg_t  *reg       = btn_find_reg(L, handle);

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

    if (!ctx->queue) {
        ctx->queue = xQueueCreate(BTN_EVENT_QUEUE_SIZE, sizeof(btn_event_item_t));
        if (!ctx->queue) {
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
        esp_err_t err = btn_set_callback(handle, ev, lua_btn_check_ud(L, 1), true);
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

static int lua_btn_off(lua_State *L)
{
    button_handle_t handle    = lua_btn_check_handle(L, 1);
    const char     *event_str = luaL_optstring(L, 2, NULL);
    btn_lua_reg_t  *reg       = btn_find_reg(L, handle);

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
            esp_err_t err = btn_set_callback(handle, ev, NULL, false);
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
                esp_err_t err = btn_set_callback(handle, (button_event_t)i, NULL, false);
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

static int lua_btn_dispatch(lua_State *L)
{
    btn_context_t *ctx = btn_context(L);
    int count = 0;
    if (!ctx->queue) {
        lua_pushinteger(L, 0);
        return 1;
    }

    btn_event_item_t item;
    while (xQueueReceive(ctx->queue, &item, 0) == pdTRUE) {
        btn_lua_reg_t *reg = btn_find_reg(L, item.handle);
        if (!reg || reg->generation != item.generation) {
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

int luaopen_button(lua_State *L)
{
    if (!btn_context(L)) {
        btn_context_t *ctx = lua_newuserdata(L, sizeof(*ctx));
        memset(ctx, 0, sizeof(*ctx));
        if (luaL_newmetatable(L, "button.context")) {
            lua_pushcfunction(L, btn_context_gc);
            lua_setfield(L, -2, "__gc");
        }
        lua_setmetatable(L, -2);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &s_context_key);
    }
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
