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

#define LUA_MODULE_DELAY_US_MAX_BLOCKING 1000000U

static int lua_module_delay_sleep_ms(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);

    if (ms < 0) {
        ms = 0;
    }

    vTaskDelay(pdMS_TO_TICKS((uint32_t)ms));
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

int luaopen_delay(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_module_delay_sleep_ms);
    lua_setfield(L, -2, "delay_ms");
    lua_pushcfunction(L, lua_module_delay_sleep_us);
    lua_setfield(L, -2, "delay_us");
    return 1;
}

esp_err_t lua_module_delay_register(void)
{
    return cap_lua_register_module("delay", luaopen_delay);
}
