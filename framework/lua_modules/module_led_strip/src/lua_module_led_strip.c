/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_led_strip.h"

#include "cap_lua.h"
#include "esp_err.h"
#include "lauxlib.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "led_strip_types.h"

#define LUA_MODULE_LED_STRIP_METATABLE "led_strip"
#define LUA_MODULE_LED_STRIP_RMT_RES_HZ (10 * 1000 * 1000)

typedef struct {
    led_strip_handle_t strip;
} lua_module_led_strip_ud_t;

static void lua_module_led_strip_hsv_to_rgb(uint32_t hue, uint32_t saturation, uint32_t value,
                                            uint32_t *red, uint32_t *green, uint32_t *blue)
{
    uint32_t region = 0;
    uint32_t remainder = 0;
    uint32_t p = 0;
    uint32_t q = 0;
    uint32_t t = 0;

    if (red) {
        *red = 0;
    }
    if (green) {
        *green = 0;
    }
    if (blue) {
        *blue = 0;
    }

    if (!red || !green || !blue) {
        return;
    }

    if (saturation == 0) {
        *red = value;
        *green = value;
        *blue = value;
        return;
    }

    hue %= 360;
    region = hue / 60;
    remainder = ((hue % 60) * 255) / 60;

    p = (value * (255 - saturation)) / 255;
    q = (value * (255 - ((saturation * remainder) / 255))) / 255;
    t = (value * (255 - ((saturation * (255 - remainder)) / 255))) / 255;

    switch (region) {
    case 0:
        *red = value;
        *green = t;
        *blue = p;
        break;
    case 1:
        *red = q;
        *green = value;
        *blue = p;
        break;
    case 2:
        *red = p;
        *green = value;
        *blue = t;
        break;
    case 3:
        *red = p;
        *green = q;
        *blue = value;
        break;
    case 4:
        *red = t;
        *green = p;
        *blue = value;
        break;
    default:
        *red = value;
        *green = p;
        *blue = q;
        break;
    }
}

static lua_module_led_strip_ud_t *lua_module_led_strip_get_ud(lua_State *L, int idx)
{
    lua_module_led_strip_ud_t *ud =
        (lua_module_led_strip_ud_t *)luaL_checkudata(L, idx, LUA_MODULE_LED_STRIP_METATABLE);
    if (!ud || !ud->strip) {
        luaL_error(L, "led_strip: invalid or closed handle");
    }
    return ud;
}

static int lua_module_led_strip_gc(lua_State *L)
{
    lua_module_led_strip_ud_t *ud =
        (lua_module_led_strip_ud_t *)luaL_testudata(L, 1, LUA_MODULE_LED_STRIP_METATABLE);
    if (ud && ud->strip) {
        led_strip_del(ud->strip);
        ud->strip = NULL;
    }
    return 0;
}

static int lua_module_led_strip_set_pixel(lua_State *L)
{
    lua_module_led_strip_ud_t *ud = lua_module_led_strip_get_ud(L, 1);
    uint32_t index = (uint32_t)luaL_checkinteger(L, 2);
    uint32_t r = (uint32_t)luaL_checkinteger(L, 3);
    uint32_t g = (uint32_t)luaL_checkinteger(L, 4);
    uint32_t b = (uint32_t)luaL_checkinteger(L, 5);
    esp_err_t err = led_strip_set_pixel(ud->strip, index, r, g, b);
    if (err != ESP_OK) {
        return luaL_error(L, "led_strip set_pixel failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_module_led_strip_set_pixel_hsv(lua_State *L)
{
    lua_module_led_strip_ud_t *ud = lua_module_led_strip_get_ud(L, 1);
    uint32_t index = (uint32_t)luaL_checkinteger(L, 2);
    uint32_t hue = (uint32_t)luaL_checkinteger(L, 3);
    uint32_t saturation = (uint32_t)luaL_checkinteger(L, 4);
    uint32_t value = (uint32_t)luaL_checkinteger(L, 5);
    uint32_t red = 0;
    uint32_t green = 0;
    uint32_t blue = 0;

    if (saturation > 255 || value > 255) {
        return luaL_error(L, "led_strip set_pixel_hsv requires s and v in range 0-255");
    }

    lua_module_led_strip_hsv_to_rgb(hue, saturation, value, &red, &green, &blue);

    esp_err_t err = led_strip_set_pixel(ud->strip, index, red, green, blue);
    if (err != ESP_OK) {
        return luaL_error(L, "led_strip set_pixel_hsv failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_module_led_strip_refresh(lua_State *L)
{
    lua_module_led_strip_ud_t *ud = lua_module_led_strip_get_ud(L, 1);
    esp_err_t err = led_strip_refresh(ud->strip);
    if (err != ESP_OK) {
        return luaL_error(L, "led_strip refresh failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_module_led_strip_clear(lua_State *L)
{
    lua_module_led_strip_ud_t *ud = lua_module_led_strip_get_ud(L, 1);
    esp_err_t err = led_strip_clear(ud->strip);
    if (err != ESP_OK) {
        return luaL_error(L, "led_strip clear failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_module_led_strip_close(lua_State *L)
{
    lua_module_led_strip_ud_t *ud =
        (lua_module_led_strip_ud_t *)luaL_checkudata(L, 1, LUA_MODULE_LED_STRIP_METATABLE);
    if (ud->strip) {
        led_strip_del(ud->strip);
        ud->strip = NULL;
    }
    return 0;
}

static int lua_module_led_strip_new(lua_State *L)
{
    int gpio = (int)luaL_checkinteger(L, 1);
    int max_leds = (int)luaL_checkinteger(L, 2);
    led_strip_handle_t strip = NULL;
    esp_err_t err;

    if (max_leds <= 0) {
        return luaL_error(L, "max_leds must be positive");
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio,
        .max_leds = (uint32_t)max_leds,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        }
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LUA_MODULE_LED_STRIP_RMT_RES_HZ,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = 0,
        }
    };

    err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (err != ESP_OK) {
        return luaL_error(L, "led_strip new failed: %s", esp_err_to_name(err));
    }

    lua_module_led_strip_ud_t *ud =
        (lua_module_led_strip_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->strip = strip;
    luaL_getmetatable(L, LUA_MODULE_LED_STRIP_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

int luaopen_led_strip(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_LED_STRIP_METATABLE)) {
        lua_pushcfunction(L, lua_module_led_strip_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_module_led_strip_set_pixel);
        lua_setfield(L, -2, "set_pixel");
        lua_pushcfunction(L, lua_module_led_strip_set_pixel_hsv);
        lua_setfield(L, -2, "set_pixel_hsv");
        lua_pushcfunction(L, lua_module_led_strip_refresh);
        lua_setfield(L, -2, "refresh");
        lua_pushcfunction(L, lua_module_led_strip_clear);
        lua_setfield(L, -2, "clear");
        lua_pushcfunction(L, lua_module_led_strip_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_module_led_strip_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_led_strip_register(void)
{
    return cap_lua_register_module("led_strip", luaopen_led_strip);
}
