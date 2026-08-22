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
    { "black",       { .r = 0,   .g = 0,   .b = 0,   .a = 255 } },
    { "white",       { .r = 255, .g = 255, .b = 255, .a = 255 } },
    { "red",         { .r = 255, .g = 0,   .b = 0,   .a = 255 } },
    { "green",       { .r = 0,   .g = 255, .b = 0,   .a = 255 } },
    { "blue",        { .r = 0,   .g = 0,   .b = 255, .a = 255 } },
    { "yellow",      { .r = 255, .g = 255, .b = 0,   .a = 255 } },
    { "cyan",        { .r = 0,   .g = 255, .b = 255, .a = 255 } },
    { "magenta",     { .r = 255, .g = 0,   .b = 255, .a = 255 } },
    { "transparent", { .r = 0,   .g = 0,   .b = 0,   .a = 0   } },
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

/* Mask that isolates R/G/B channels when a 565 pixel is spread across a
   32-bit word as (pixel << 16) | pixel; enables single-multiply blending
   without per-channel divides. */
#define DISPLAY_COLOR_RGB565_CHANNEL_MASK 0x07E0F81FU

uint16_t display_color_blend_rgb565(uint16_t dst, display_color_t src)
{
    if (src.a == 0) {
        return dst;
    }
    if (src.a == 255) {
        return display_color_to_rgb565(src);
    }

    /* Widen alpha to 6-bit (0..64), +2 for round-to-nearest. */
    uint32_t a5 = ((uint32_t)src.a + 2) >> 2;
    if (a5 > 64) {
        a5 = 64;
    }

    uint16_t src565 = display_color_to_rgb565(src);
    uint32_t bg32 = ((uint32_t)dst | ((uint32_t)dst << 16)) & DISPLAY_COLOR_RGB565_CHANNEL_MASK;
    uint32_t fg32 = ((uint32_t)src565 | ((uint32_t)src565 << 16)) & DISPLAY_COLOR_RGB565_CHANNEL_MASK;
    uint32_t out = (bg32 + (((fg32 - bg32) * a5) >> 6)) & DISPLAY_COLOR_RGB565_CHANNEL_MASK;
    return (uint16_t)((out >> 16) | out);
}

uint32_t display_color_to_rgb888(display_color_t color)
{
    return ((uint32_t)color.b << 16) | ((uint32_t)color.g << 8) | (uint32_t)color.r;
}

/* Exact x/255 for x in [0..65534] via mul+shift (Xtensa has no HW divide). */
static inline uint8_t display_color_div255(uint32_t x)
{
    return (uint8_t)((x * 0x8081U) >> 23);
}

uint32_t display_color_blend_rgb888(uint32_t dst, display_color_t src)
{
    if (src.a == 0) {
        return dst;
    }
    if (src.a == 255) {
        return display_color_to_rgb888(src);
    }

    uint32_t a = src.a;
    uint32_t inv_a = 255U - a;
    uint32_t dst_b = (dst >> 16) & 0xFFU;
    uint32_t dst_g = (dst >> 8) & 0xFFU;
    uint32_t dst_r = dst & 0xFFU;

    /* +127 for round-to-nearest. */
    uint32_t b = display_color_div255((uint32_t)src.b * a + dst_b * inv_a + 127U);
    uint32_t g = display_color_div255((uint32_t)src.g * a + dst_g * inv_a + 127U);
    uint32_t r = display_color_div255((uint32_t)src.r * a + dst_r * inv_a + 127U);
    return (b << 16) | (g << 8) | r;
}

bool display_color_is_transparent(display_color_t color)
{
    return color.a == 0;
}

bool display_color_is_opaque(display_color_t color)
{
    return color.a == 255;
}
