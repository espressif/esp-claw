/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_gpio.h"

#include <string.h>

#include "cap_lua.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "lauxlib.h"

static gpio_mode_t lua_driver_gpio_mode_from_string(const char *mode)
{
    if (!mode || strcmp(mode, "input") == 0) {
        return GPIO_MODE_INPUT;
    }
    if (strcmp(mode, "output") == 0) {
        return GPIO_MODE_OUTPUT;
    }
    if (strcmp(mode, "input_output") == 0) {
        return GPIO_MODE_INPUT_OUTPUT;
    }
    if (strcmp(mode, "output_od") == 0) {
        return GPIO_MODE_OUTPUT_OD;
    }
    if (strcmp(mode, "input_output_od") == 0) {
        return GPIO_MODE_INPUT_OUTPUT_OD;
    }
    return GPIO_MODE_DISABLE;
}

static int lua_driver_gpio_set_direction(lua_State *L)
{
    gpio_num_t pin = (gpio_num_t)luaL_checkinteger(L, 1);
    const char *mode_str = luaL_checkstring(L, 2);
    gpio_mode_t mode = lua_driver_gpio_mode_from_string(mode_str);

    if (mode == GPIO_MODE_DISABLE && strcmp(mode_str, "disable") != 0) {
        return luaL_error(L, "invalid gpio mode: %s", mode_str);
    }

    if (gpio_set_direction(pin, mode) != ESP_OK) {
        return luaL_error(L, "gpio_set_direction failed");
    }

    return 0;
}

static int lua_driver_gpio_set_level(lua_State *L)
{
    gpio_num_t pin = (gpio_num_t)luaL_checkinteger(L, 1);
    uint32_t level = (uint32_t)luaL_checkinteger(L, 2);

    if (gpio_set_level(pin, level ? 1 : 0) != ESP_OK) {
        return luaL_error(L, "gpio_set_level failed");
    }

    return 0;
}

static int lua_driver_gpio_get_level(lua_State *L)
{
    gpio_num_t pin = (gpio_num_t)luaL_checkinteger(L, 1);

    lua_pushinteger(L, gpio_get_level(pin));
    return 1;
}

int luaopen_gpio(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_gpio_set_direction);
    lua_setfield(L, -2, "set_direction");
    lua_pushcfunction(L, lua_driver_gpio_set_level);
    lua_setfield(L, -2, "set_level");
    lua_pushcfunction(L, lua_driver_gpio_get_level);
    lua_setfield(L, -2, "get_level");
    return 1;
}

esp_err_t lua_driver_gpio_register(void)
{
    return cap_lua_register_module("gpio", luaopen_gpio);
}
