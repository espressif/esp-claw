/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_color.h"

#include <stddef.h>
#include <string.h>
#include "esp_log.h"
#include "lauxlib.h"

static const char *TAG = "display_color";

typedef struct {
    const char *name;
    display_color_t color;
} display_named_color_t;

static const display_named_color_t s_named_colors[] = {
    { "black",       { 0,   0,   0,   255 } },
    { "white",       { 255, 255, 255, 255 } },
    { "red",         { 255, 0,   0,   255 } },
    { "green",       { 0,   255, 0,   255 } },
    { "blue",        { 0,   0,   255, 255 } },
    { "yellow",      { 255, 255, 0,   255 } },
    { "cyan",        { 0,   255, 255, 255 } },
    { "magenta",     { 255, 0,   255, 255 } },
    { "transparent", { 0,   0,   0,   0   } },
};

static int display_color_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static uint8_t display_color_expand_nibble(int value)
{
    return (uint8_t)((value << 4) | value);
}

static esp_err_t display_color_parse_hex(lua_State *L, int index, const char *value, display_color_t *out_color)
{
    size_t len = strlen(value);

    if (len != 4 && len != 5 && len != 7 && len != 9) {
        ESP_LOGE(TAG, "invalid hex color length: %u", (unsigned)len);
        return luaL_error(L, "display color hex string must be #rgb, #rgba, #rrggbb, or #rrggbbaa");
    }

    if (len == 4 || len == 5) {
        int r = display_color_hex_value(value[1]);
        int g = display_color_hex_value(value[2]);
        int b = display_color_hex_value(value[3]);
        int a = (len == 5) ? display_color_hex_value(value[4]) : 15;

        if (r < 0 || g < 0 || b < 0 || a < 0) {
            ESP_LOGE(TAG, "invalid short hex color");
            return luaL_argerror(L, index, "display color contains a non-hex digit");
        }
        *out_color = (display_color_t) {
            .r = display_color_expand_nibble(r),
            .g = display_color_expand_nibble(g),
            .b = display_color_expand_nibble(b),
            .a = display_color_expand_nibble(a),
        };
        return ESP_OK;
    }

    uint8_t components[4] = { 0, 0, 0, 255 };
    int component_count = (len == 9) ? 4 : 3;
    for (int i = 0; i < component_count; ++i) {
        int hi = display_color_hex_value(value[1 + i * 2]);
        int lo = display_color_hex_value(value[2 + i * 2]);
        if (hi < 0 || lo < 0) {
            ESP_LOGE(TAG, "invalid long hex color");
            return luaL_argerror(L, index, "display color contains a non-hex digit");
        }
        components[i] = (uint8_t)((hi << 4) | lo);
    }

    *out_color = (display_color_t) {
        .r = components[0],
        .g = components[1],
        .b = components[2],
        .a = components[3],
    };
    return ESP_OK;
}

static bool display_color_parse_named(const char *value, display_color_t *out_color)
{
    for (size_t i = 0; i < sizeof(s_named_colors) / sizeof(s_named_colors[0]); ++i) {
        if (strcmp(value, s_named_colors[i].name) == 0) {
            *out_color = s_named_colors[i].color;
            return true;
        }
    }
    return false;
}

static uint8_t display_color_check_component(lua_State *L, int index, const char *name)
{
    lua_getfield(L, index, name);
    if (!lua_isinteger(L, -1)) {
        ESP_LOGE(TAG, "missing or invalid color component: %s", name);
        luaL_error(L, "display color component '%s' must be an integer", name);
    }
    lua_Integer value = lua_tointeger(L, -1);
    lua_pop(L, 1);

    if (value < 0 || value > 255) {
        ESP_LOGE(TAG, "color component out of range: %s=%d", name, (int)value);
        luaL_error(L, "display color component '%s' must be in [0, 255]", name);
    }
    return (uint8_t)value;
}

static uint8_t display_color_opt_component(lua_State *L, int index, const char *name, uint8_t default_value)
{
    lua_getfield(L, index, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return default_value;
    }
    if (!lua_isinteger(L, -1)) {
        ESP_LOGE(TAG, "invalid optional color component: %s", name);
        luaL_error(L, "display color component '%s' must be an integer", name);
    }
    lua_Integer value = lua_tointeger(L, -1);
    lua_pop(L, 1);

    if (value < 0 || value > 255) {
        ESP_LOGE(TAG, "optional color component out of range: %s=%d", name, (int)value);
        luaL_error(L, "display color component '%s' must be in [0, 255]", name);
    }
    return (uint8_t)value;
}

esp_err_t display_color_from_lua(lua_State *L, int index, display_color_t *out_color)
{
    if (!out_color) {
        ESP_LOGE(TAG, "color output is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (lua_type(L, index) == LUA_TSTRING) {
        const char *value = lua_tostring(L, index);
        if (!value) {
            ESP_LOGE(TAG, "color string is NULL");
            return luaL_argerror(L, index, "display color string is NULL");
        }
        if (value[0] == '#') {
            return display_color_parse_hex(L, index, value, out_color);
        }
        if (display_color_parse_named(value, out_color)) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "unknown named color: %s", value);
        return luaL_error(L, "unknown display color '%s'", value);
    }

    if (lua_istable(L, index)) {
        *out_color = (display_color_t) {
            .r = display_color_check_component(L, index, "r"),
            .g = display_color_check_component(L, index, "g"),
            .b = display_color_check_component(L, index, "b"),
            .a = display_color_opt_component(L, index, "a", 255),
        };
        return ESP_OK;
    }

    ESP_LOGE(TAG, "invalid color argument type: %s", luaL_typename(L, index));
    return luaL_argerror(L, index, "display color must be a string or table");
}

uint16_t display_color_to_rgb565(display_color_t color)
{
    return (uint16_t)(((color.r & 0xF8) << 8) | ((color.g & 0xFC) << 3) | ((color.b & 0xF8) >> 3));
}

uint16_t display_color_blend_rgb565(uint16_t dst, display_color_t src)
{
    uint8_t dst_r = (uint8_t)(((dst >> 11) & 0x1F) * 255 / 31);
    uint8_t dst_g = (uint8_t)(((dst >> 5) & 0x3F) * 255 / 63);
    uint8_t dst_b = (uint8_t)((dst & 0x1F) * 255 / 31);
    uint16_t inv_a = (uint16_t)(255 - src.a);
    display_color_t blended = {
        .r = (uint8_t)(((uint16_t)src.r * src.a + (uint16_t)dst_r * inv_a + 127) / 255),
        .g = (uint8_t)(((uint16_t)src.g * src.a + (uint16_t)dst_g * inv_a + 127) / 255),
        .b = (uint8_t)(((uint16_t)src.b * src.a + (uint16_t)dst_b * inv_a + 127) / 255),
        .a = 255,
    };

    return display_color_to_rgb565(blended);
}

bool display_color_is_transparent(display_color_t color)
{
    return color.a == 0;
}

bool display_color_is_opaque(display_color_t color)
{
    return color.a == 255;
}
