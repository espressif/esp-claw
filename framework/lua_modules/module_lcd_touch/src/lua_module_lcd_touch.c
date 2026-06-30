/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_lcd_touch.h"

#include <stdbool.h>
#include <stdint.h>

#include "cap_lua.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *LUA_LCD_TOUCH_STATE_KEY = "esp-claw.lcd_touch_state";

static esp_lcd_touch_handle_t lua_lcd_touch_check_handle(lua_State *L, int index)
{
    void *handle = lua_touserdata(L, index);
    luaL_argcheck(L, handle != NULL, index, "lcd_touch handle expected");
    return (esp_lcd_touch_handle_t)handle;
}

/*
 * 与 setup_device 的契约 (board 通用):
 *   - touch_handle->config.interrupt_callback != NULL  =>  中断驱动模式
 *   - touch_handle->config.user_data                   =>  SemaphoreHandle_t (二值)
 *
 * 中断驱动模式下,本模块在首次见到该 handle 时启动一个后台 task,阻塞在
 * 信号量上;ISR give 一次即完成一次 esp_lcd_touch_read_data 把数据写入
 * tp->data 缓存。Lua 端的 read/poll/sync 只消费缓存 (esp_lcd_touch_get_data),
 * 不再亲自发 I2C —— "收到中断就读,用不用是 lua 的事"。
 *
 * 没挂 interrupt_callback 的板子 (例如 CST816S 走轮询) 维持原有行为不变。
 */
#define LUA_LCD_TOUCH_MAX_INT_HANDLES   2
#define LUA_LCD_TOUCH_INT_TASK_STACK    4096
#define LUA_LCD_TOUCH_INT_TASK_PRIO     5

static const char *LUA_LCD_TOUCH_TAG = "lua_lcd_touch";

static esp_lcd_touch_handle_t s_int_handles[LUA_LCD_TOUCH_MAX_INT_HANDLES];
static portMUX_TYPE s_int_handles_mux = portMUX_INITIALIZER_UNLOCKED;

static void lua_lcd_touch_int_task(void *arg)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)arg;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)tp->config.user_data;
    while (1) {
        if (xSemaphoreTake(sem, portMAX_DELAY) == pdTRUE) {
            (void)esp_lcd_touch_read_data(tp);
        }
    }
}

static void lua_lcd_touch_ensure_int_task(esp_lcd_touch_handle_t tp)
{
    if (tp->config.interrupt_callback == NULL || tp->config.user_data == NULL) {
        return;
    }

    bool already_started = false;
    int free_slot = -1;
    portENTER_CRITICAL(&s_int_handles_mux);
    for (int i = 0; i < LUA_LCD_TOUCH_MAX_INT_HANDLES; i++) {
        if (s_int_handles[i] == tp) {
            already_started = true;
            break;
        }
        if (s_int_handles[i] == NULL && free_slot < 0) {
            free_slot = i;
        }
    }
    if (!already_started && free_slot >= 0) {
        s_int_handles[free_slot] = tp;
    }
    portEXIT_CRITICAL(&s_int_handles_mux);

    if (already_started) {
        return;
    }
    if (free_slot < 0) {
        ESP_LOGE(LUA_LCD_TOUCH_TAG, "no slot for interrupt-driven touch handle %p", tp);
        return;
    }

    if (xTaskCreate(lua_lcd_touch_int_task, "lcd_touch_int", LUA_LCD_TOUCH_INT_TASK_STACK,
                    tp, LUA_LCD_TOUCH_INT_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(LUA_LCD_TOUCH_TAG, "failed to start interrupt task for handle %p", tp);
        portENTER_CRITICAL(&s_int_handles_mux);
        s_int_handles[free_slot] = NULL;
        portEXIT_CRITICAL(&s_int_handles_mux);
    }
}

static esp_err_t lua_lcd_touch_read_raw(esp_lcd_touch_handle_t touch_handle,
                                        bool *pressed, uint16_t *x, uint16_t *y)
{
    esp_lcd_touch_point_data_t points[1];
    uint8_t point_count = 0;
    esp_err_t err;

    if (touch_handle == NULL || pressed == NULL || x == NULL || y == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (touch_handle->config.interrupt_callback != NULL) {
        lua_lcd_touch_ensure_int_task(touch_handle);
    } else {
        (void)esp_lcd_touch_read_data(touch_handle);
    }

    err = esp_lcd_touch_get_data(touch_handle, points, &point_count, 1);
    if (err != ESP_OK) {
        return err;
    }

    *pressed = point_count > 0;
    if (*pressed) {
        *x = points[0].x;
        *y = points[0].y;
    } else {
        *x = 0;
        *y = 0;
    }

    return ESP_OK;
}

static void lua_lcd_touch_push_touch_table(lua_State *L,
                                           bool pressed,
                                           bool just_pressed,
                                           bool just_released,
                                           uint16_t x,
                                           uint16_t y,
                                           int dx,
                                           int dy,
                                           lua_Number held_ms)
{
    lua_newtable(L);

    lua_pushboolean(L, pressed);
    lua_setfield(L, -2, "pressed");

    lua_pushboolean(L, just_pressed);
    lua_setfield(L, -2, "just_pressed");

    lua_pushboolean(L, just_released);
    lua_setfield(L, -2, "just_released");

    lua_pushinteger(L, x);
    lua_setfield(L, -2, "x");

    lua_pushinteger(L, y);
    lua_setfield(L, -2, "y");

    lua_pushinteger(L, dx);
    lua_setfield(L, -2, "dx");

    lua_pushinteger(L, dy);
    lua_setfield(L, -2, "dy");

    lua_pushboolean(L, (dx != 0) || (dy != 0));
    lua_setfield(L, -2, "moved");

    lua_pushnumber(L, held_ms);
    lua_setfield(L, -2, "held_ms");
}

static void lua_lcd_touch_push_state_table(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LCD_TOUCH_STATE_KEY);
    if (lua_istable(L, -1)) {
        return;
    }

    lua_pop(L, 1);
    lua_newtable(L);

    lua_pushboolean(L, false);
    lua_setfield(L, -2, "initialized");

    lua_pushboolean(L, false);
    lua_setfield(L, -2, "pressed");

    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "x");

    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "y");

    lua_pushnumber(L, 0);
    lua_setfield(L, -2, "press_start_ms");

    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, LUA_LCD_TOUCH_STATE_KEY);
}

static void lua_lcd_touch_update_state(lua_State *L,
                                       bool initialized,
                                       bool pressed,
                                       uint16_t x,
                                       uint16_t y,
                                       lua_Number press_start_ms)
{
    lua_pushboolean(L, initialized);
    lua_setfield(L, -2, "initialized");

    lua_pushboolean(L, pressed);
    lua_setfield(L, -2, "pressed");

    lua_pushinteger(L, x);
    lua_setfield(L, -2, "x");

    lua_pushinteger(L, y);
    lua_setfield(L, -2, "y");

    lua_pushnumber(L, press_start_ms);
    lua_setfield(L, -2, "press_start_ms");
}

static int lua_lcd_touch_sync(lua_State *L)
{
    esp_lcd_touch_handle_t touch_handle = lua_lcd_touch_check_handle(L, 1);
    bool pressed = false;
    uint16_t x = 0;
    uint16_t y = 0;
    lua_Number now_ms = (lua_Number)(esp_timer_get_time() / 1000);
    esp_err_t err = lua_lcd_touch_read_raw(touch_handle, &pressed, &x, &y);
    if (err != ESP_OK) {
        return luaL_error(L, "lcd_touch sync failed: %s", esp_err_to_name(err));
    }

    lua_lcd_touch_push_state_table(L);
    lua_lcd_touch_update_state(L, true, pressed, x, y, pressed ? now_ms : 0);
    lua_pop(L, 1);

    lua_lcd_touch_push_touch_table(L, pressed, false, false, x, y, 0, 0, 0);
    return 1;
}

static int lua_lcd_touch_read(lua_State *L)
{
    esp_lcd_touch_handle_t touch_handle = lua_lcd_touch_check_handle(L, 1);
    bool pressed = false;
    uint16_t x = 0;
    uint16_t y = 0;
    esp_err_t err = lua_lcd_touch_read_raw(touch_handle, &pressed, &x, &y);
    if (err != ESP_OK) {
        return luaL_error(L, "lcd_touch read failed: %s", esp_err_to_name(err));
    }

    lua_newtable(L);
    lua_pushboolean(L, pressed);
    lua_setfield(L, -2, "pressed");
    if (pressed) {
        lua_pushinteger(L, x);
        lua_setfield(L, -2, "x");
        lua_pushinteger(L, y);
        lua_setfield(L, -2, "y");
    }
    return 1;
}

static int lua_lcd_touch_poll(lua_State *L)
{
    esp_lcd_touch_handle_t touch_handle = lua_lcd_touch_check_handle(L, 1);
    bool pressed = false;
    bool prev_pressed = false;
    bool initialized = false;
    bool just_pressed = false;
    bool just_released = false;
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t prev_x = 0;
    uint16_t prev_y = 0;
    int dx = 0;
    int dy = 0;
    lua_Number press_start_ms = 0;
    lua_Number held_ms = 0;
    lua_Number now_ms = (lua_Number)(esp_timer_get_time() / 1000);
    esp_err_t err = lua_lcd_touch_read_raw(touch_handle, &pressed, &x, &y);
    if (err != ESP_OK) {
        return luaL_error(L, "lcd_touch poll failed: %s", esp_err_to_name(err));
    }

    lua_lcd_touch_push_state_table(L);

    lua_getfield(L, -1, "initialized");
    initialized = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "pressed");
    prev_pressed = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "x");
    prev_x = (uint16_t)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "y");
    prev_y = (uint16_t)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "press_start_ms");
    press_start_ms = lua_tonumber(L, -1);
    lua_pop(L, 1);

    if (!initialized) {
        if (pressed) {
            press_start_ms = now_ms;
        } else {
            x = 0;
            y = 0;
            press_start_ms = 0;
        }
    } else if (pressed && prev_pressed) {
        dx = (int)x - (int)prev_x;
        dy = (int)y - (int)prev_y;
        held_ms = press_start_ms > 0 ? (now_ms - press_start_ms) : 0;
    } else if (pressed && !prev_pressed) {
        just_pressed = true;
        press_start_ms = now_ms;
    } else if (!pressed && prev_pressed) {
        just_released = true;
        x = prev_x;
        y = prev_y;
        press_start_ms = 0;
    } else {
        x = 0;
        y = 0;
        press_start_ms = 0;
    }

    if (pressed && held_ms <= 0 && press_start_ms > 0) {
        held_ms = now_ms - press_start_ms;
        if (held_ms < 0) {
            held_ms = 0;
        }
    }

    lua_lcd_touch_update_state(L, true, pressed, x, y, press_start_ms);
    lua_pop(L, 1);

    lua_lcd_touch_push_touch_table(L, pressed, just_pressed, just_released, x, y, dx, dy,
                                   held_ms);
    return 1;
}

int luaopen_lcd_touch(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_lcd_touch_read);
    lua_setfield(L, -2, "read");
    lua_pushcfunction(L, lua_lcd_touch_poll);
    lua_setfield(L, -2, "poll");
    lua_pushcfunction(L, lua_lcd_touch_sync);
    lua_setfield(L, -2, "sync");
    return 1;
}

esp_err_t lua_module_lcd_touch_register(void)
{
    return cap_lua_register_module("lcd_touch", luaopen_lcd_touch);
}
