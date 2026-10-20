/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_delay.h"

#include <stdint.h>

#include "cap_lua.h"
#include "esp_rom_sys.h"
#include "lauxlib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LUA_MODULE_DELAY_PERIODIC_MT "delay.periodic"
#define LUA_MODULE_DELAY_US_MAX_BLOCKING 1000000U
#define LUA_MODULE_DELAY_MS_STOP_SLICE 100U

typedef struct {
    TickType_t last_wake_time;
    TickType_t period_ticks;
} lua_delay_periodic_t;

static int lua_module_delay_sleep_ms(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    uint32_t remaining;

    if (ms < 0) {
        ms = 0;
    }

    remaining = (uint32_t)ms;
    while (remaining > 0) {
        uint32_t step = remaining > LUA_MODULE_DELAY_MS_STOP_SLICE ?
                        LUA_MODULE_DELAY_MS_STOP_SLICE : remaining;
        if (cap_lua_runtime_stop_requested(L)) {
            return luaL_error(L, "stop requested");
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        remaining -= step;
    }
    if (cap_lua_runtime_stop_requested(L)) {
        return luaL_error(L, "stop requested");
    }
    return 0;
}

static int lua_module_delay_sleep_us(lua_State *L)
{
    lua_Integer us = luaL_checkinteger(L, 1);

    if (us < 0) {
        us = 0;
    }

    if ((uint64_t)us > LUA_MODULE_DELAY_US_MAX_BLOCKING) {
        return luaL_error(L, "delay_us supports 0..%u only; use delay_ms for longer waits",
                          LUA_MODULE_DELAY_US_MAX_BLOCKING);
    }

    /* Microsecond delay is a busy-wait, so keep it for short hardware timings only. */
    esp_rom_delay_us((uint32_t)us);
    return 0;
}

static lua_delay_periodic_t *lua_module_delay_check_periodic(lua_State *L, int index)
{
    return (lua_delay_periodic_t *)luaL_checkudata(L, index, LUA_MODULE_DELAY_PERIODIC_MT);
}

static int lua_module_delay_periodic_wait(lua_State *L)
{
    lua_delay_periodic_t *periodic = lua_module_delay_check_periodic(L, 1);
    if (cap_lua_runtime_stop_requested(L)) {
        return luaL_error(L, "stop requested");
    }
    BaseType_t delayed = xTaskDelayUntil(&periodic->last_wake_time, periodic->period_ticks);
    if (cap_lua_runtime_stop_requested(L)) {
        return luaL_error(L, "stop requested");
    }
    lua_pushboolean(L, delayed == pdTRUE);
    return 1;
}

static int lua_module_delay_periodic_reset(lua_State *L)
{
    lua_delay_periodic_t *periodic = lua_module_delay_check_periodic(L, 1);
    periodic->last_wake_time = xTaskGetTickCount();
    return 0;
}

static int lua_module_delay_periodic(lua_State *L)
{
    lua_Integer period_ms = luaL_checkinteger(L, 1);
    if (period_ms <= 0 || (uint64_t)period_ms > UINT32_MAX) {
        return luaL_error(L, "period_ms must be in range 1..%u", UINT32_MAX);
    }
    TickType_t period_ticks = pdMS_TO_TICKS((uint32_t)period_ms);
    if (period_ticks == 0) {
        return luaL_error(L, "period_ms is shorter than one FreeRTOS tick");
    }

    lua_delay_periodic_t *periodic = (lua_delay_periodic_t *)lua_newuserdata(L, sizeof(*periodic));
    periodic->last_wake_time = xTaskGetTickCount();
    periodic->period_ticks = period_ticks;
    luaL_getmetatable(L, LUA_MODULE_DELAY_PERIODIC_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static void lua_module_delay_register_periodic(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_DELAY_PERIODIC_MT)) {
        static const luaL_Reg methods[] = {
            {"wait", lua_module_delay_periodic_wait},
            {"reset", lua_module_delay_periodic_reset},
            {NULL, NULL},
        };
        lua_newtable(L);
        luaL_setfuncs(L, methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

int luaopen_delay(lua_State *L)
{
    lua_module_delay_register_periodic(L);
    lua_newtable(L);
    lua_pushcfunction(L, lua_module_delay_sleep_ms);
    lua_setfield(L, -2, "delay_ms");
    lua_pushcfunction(L, lua_module_delay_sleep_us);
    lua_setfield(L, -2, "delay_us");
    lua_pushcfunction(L, lua_module_delay_periodic);
    lua_setfield(L, -2, "periodic");
    return 1;
}

esp_err_t lua_module_delay_register(void)
{
    return cap_lua_register_module("delay", luaopen_delay);
}
