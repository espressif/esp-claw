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
#include "esp_log.h"
#include "lauxlib.h"
#include "cap_lua_hw.h"
#include "soc/soc_caps.h"

typedef struct {
    cap_lua_hw_lease_t lease;
    int pin;
} lua_gpio_t;

static const char s_gpio_key;

static esp_err_t lua_gpio_close(lua_gpio_t *gpio)
{
    if (!gpio->lease.id) return ESP_OK;
    esp_err_t err = gpio_reset_pin(gpio->pin);
    return err == ESP_OK ? cap_lua_hw_release(&gpio->lease) : err;
}

static int lua_gpio_gc(lua_State *L)
{
    lua_gpio_t *gpio = lua_touserdata(L, 1);
    esp_err_t err = lua_gpio_close(gpio);
    if (err != ESP_OK) ESP_LOGE("lua_gpio", "Cleanup failed: %s", esp_err_to_name(err));
    return 0;
}

static lua_gpio_t *lua_gpio_claim(lua_State *L, int pin)
{
    luaL_argcheck(L, pin >= 0 && pin < SOC_GPIO_PIN_COUNT && GPIO_IS_VALID_GPIO(pin), 1, "invalid GPIO");
    lua_rawgetp(L, LUA_REGISTRYINDEX, &s_gpio_key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &s_gpio_key);
    }
    lua_rawgeti(L, -1, pin);
    lua_gpio_t *gpio = lua_touserdata(L, -1);
    if (!gpio) {
        lua_pop(L, 1);
        gpio = lua_newuserdata(L, sizeof(*gpio));
        *gpio = (lua_gpio_t) { .pin = pin };
        if (luaL_newmetatable(L, "gpio.pin")) {
            lua_pushcfunction(L, lua_gpio_gc);
            lua_setfield(L, -2, "__gc");
        }
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, pin);
    }
    if (!gpio->lease.id) {
        esp_err_t err = cap_lua_hw_claim_gpio(L, &gpio->lease, "lua/gpio", pin);
        if (err != ESP_OK) luaL_error(L, "gpio claim failed: %s", esp_err_to_name(err));
    }
    lua_pop(L, 2);
    return gpio;
}

static int lua_driver_gpio_close(lua_State *L)
{
    int pin = luaL_checkinteger(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &s_gpio_key);
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, pin);
        lua_gpio_t *gpio = lua_touserdata(L, -1);
        esp_err_t err = gpio ? lua_gpio_close(gpio) : ESP_OK;
        if (err != ESP_OK) return luaL_error(L, "gpio close failed: %s", esp_err_to_name(err));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return 0;
}

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

    lua_gpio_claim(L, pin);
    if (gpio_set_direction(pin, mode) != ESP_OK) {
        return luaL_error(L, "gpio_set_direction failed");
    }

    return 0;
}

static int lua_driver_gpio_set_level(lua_State *L)
{
    gpio_num_t pin = (gpio_num_t)luaL_checkinteger(L, 1);
    uint32_t level = (uint32_t)luaL_checkinteger(L, 2);

    lua_gpio_claim(L, pin);
    if (gpio_set_level(pin, level ? 1 : 0) != ESP_OK) {
        return luaL_error(L, "gpio_set_level failed");
    }

    return 0;
}

static int lua_driver_gpio_get_level(lua_State *L)
{
    gpio_num_t pin = (gpio_num_t)luaL_checkinteger(L, 1);

    lua_gpio_claim(L, pin);
    lua_pushinteger(L, gpio_get_level(pin));
    return 1;
}

int luaopen_gpio(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_gpio_close);
    lua_setfield(L, -2, "close");
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
