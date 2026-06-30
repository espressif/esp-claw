/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_text.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "display_color.h"
#include "display_hal.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"

static const char *TAG = "display_text";

typedef struct {
    display_color_t color;
    display_color_t bg;
    bool has_bg;
    uint8_t font_size;
    display_hal_text_align_t align;
    display_hal_text_valign_t valign;
} display_text_style_t;

static int display_text_check_integer_arg(lua_State *L, int index, const char *name)
{
    if (!lua_isinteger(L, index)) {
        ESP_LOGE(TAG, "%s is not an integer", name);
        return luaL_error(L, "display %s must be an integer", name);
    }
    return (int)lua_tointeger(L, index);
}

static uint8_t display_text_check_font_size(lua_State *L, int index)
{
    int font_size = display_text_check_integer_arg(L, index, "font_size");
    if (font_size <= 0 || font_size > UINT8_MAX) {
        ESP_LOGE(TAG, "font_size out of range: %d", font_size);
        luaL_error(L, "display font_size must be between 1 and 255");
    }
    return (uint8_t)font_size;
}

static bool display_text_is_ascii(const char *text)
{
    if (!text) {
        return false;
    }
    while (*text) {
        unsigned char ch = (unsigned char)*text++;
        if ((ch < 32 || ch > 126) && ch != '\n' && ch != '\r' && ch != '\t') {
            return false;
        }
    }
    return true;
}

static void display_text_reject_table_field(lua_State *L, int index, const char *field)
{
    lua_getfield(L, index, field);
    bool exists = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (exists) {
        luaL_error(L, "display text options no longer supports '%s'; use color or bg", field);
    }
}

static display_hal_text_align_t display_text_parse_align(lua_State *L, int index, display_hal_text_align_t default_align)
{
    const char *value = lua_tostring(L, index);
    if (!value || strcmp(value, "left") == 0) {
        return DISPLAY_HAL_TEXT_ALIGN_LEFT;
    }
    if (strcmp(value, "center") == 0 || strcmp(value, "centre") == 0) {
        return DISPLAY_HAL_TEXT_ALIGN_CENTER;
    }
    if (strcmp(value, "right") == 0) {
        return DISPLAY_HAL_TEXT_ALIGN_RIGHT;
    }
    luaL_error(L, "display align must be left, center, or right");
    return default_align;
}

static display_hal_text_valign_t display_text_parse_valign(lua_State *L, int index, display_hal_text_valign_t default_valign)
{
    const char *value = lua_tostring(L, index);
    if (!value || strcmp(value, "top") == 0) {
        return DISPLAY_HAL_TEXT_VALIGN_TOP;
    }
    if (strcmp(value, "middle") == 0 || strcmp(value, "center") == 0) {
        return DISPLAY_HAL_TEXT_VALIGN_MIDDLE;
    }
    if (strcmp(value, "bottom") == 0) {
        return DISPLAY_HAL_TEXT_VALIGN_BOTTOM;
    }
    luaL_error(L, "display valign must be top, middle, or bottom");
    return default_valign;
}

static void display_text_style_init_default(display_text_style_t *style)
{
    if (!style) {
        ESP_LOGE(TAG, "text style output is NULL");
        return;
    }

    /* Keep defaults centralized so every text API behaves consistently. */
    style->color = (display_color_t) {
        .r = 255,
        .g = 255,
        .b = 255,
        .a = 255,
    };
    style->bg = (display_color_t) {
        .r = 0,
        .g = 0,
        .b = 0,
        .a = 255,
    };
    style->has_bg = false;
    style->font_size = 24;
    style->align = DISPLAY_HAL_TEXT_ALIGN_LEFT;
    style->valign = DISPLAY_HAL_TEXT_VALIGN_TOP;
}

static void display_text_style_from_lua(lua_State *L, int index, display_text_style_t *style)
{
    display_text_style_init_default(style);
    if (lua_isnoneornil(L, index)) {
        return;
    }

    luaL_checktype(L, index, LUA_TTABLE);
    display_text_reject_table_field(L, index, "r");
    display_text_reject_table_field(L, index, "g");
    display_text_reject_table_field(L, index, "b");
    display_text_reject_table_field(L, index, "bg_r");
    display_text_reject_table_field(L, index, "bg_g");
    display_text_reject_table_field(L, index, "bg_b");

    lua_getfield(L, index, "color");
    if (!lua_isnil(L, -1)) {
        esp_err_t err = display_color_from_lua(L, -1, &style->color);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "text color parse failed: %s", esp_err_to_name(err));
            luaL_error(L, "display text color invalid: %s", esp_err_to_name(err));
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "font_size");
    if (!lua_isnil(L, -1)) {
        style->font_size = display_text_check_font_size(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "bg");
    if (!lua_isnil(L, -1)) {
        esp_err_t err = display_color_from_lua(L, -1, &style->bg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "text background parse failed: %s", esp_err_to_name(err));
            luaL_error(L, "display text background invalid: %s", esp_err_to_name(err));
        }
        style->has_bg = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "align");
    if (!lua_isnil(L, -1)) {
        style->align = display_text_parse_align(L, -1, style->align);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "valign");
    if (!lua_isnil(L, -1)) {
        style->valign = display_text_parse_valign(L, -1, style->valign);
    }
    lua_pop(L, 1);
}

static int display_text_draw(lua_State *L)
{
    int x = display_text_check_integer_arg(L, 1, "x");
    int y = display_text_check_integer_arg(L, 2, "y");
    const char *text = luaL_checkstring(L, 3);
    display_text_style_t style;

    if (!display_text_is_ascii(text)) {
        return luaL_error(L, "display draw_text only supports ASCII text");
    }

    display_text_style_from_lua(L, 4, &style);

    esp_err_t err = display_hal_draw_text(x, y, text, style.font_size, style.color, style.has_bg, style.bg);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_text failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int display_text_measure(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);
    display_text_style_t style;
    uint16_t width = 0;
    uint16_t height = 0;

    display_text_style_from_lua(L, 2, &style);
    if (!display_text_is_ascii(text)) {
        return luaL_error(L, "display measure_text only supports ASCII text");
    }

    esp_err_t err = display_hal_measure_text(text, style.font_size, &width, &height);
    if (err != ESP_OK) {
        return luaL_error(L, "display measure_text failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, width);
    lua_pushinteger(L, height);
    return 2;
}

static int display_text_draw_aligned(lua_State *L)
{
    int x = display_text_check_integer_arg(L, 1, "x");
    int y = display_text_check_integer_arg(L, 2, "y");
    int width = display_text_check_integer_arg(L, 3, "width");
    int height = display_text_check_integer_arg(L, 4, "height");
    const char *text = luaL_checkstring(L, 5);
    display_text_style_t style;

    if (!display_text_is_ascii(text)) {
        return luaL_error(L, "display draw_text_aligned only supports ASCII text");
    }

    display_text_style_from_lua(L, 6, &style);

    esp_err_t err = display_hal_draw_text_aligned(x, y, width, height, text, style.font_size,
                                                  style.color, style.has_bg, style.bg, style.align, style.valign);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_text_aligned failed: %s", esp_err_to_name(err));
    }
    return 0;
}

void display_text_register_lua(lua_State *L)
{
    lua_pushcfunction(L, display_text_measure);
    lua_setfield(L, -2, "measure_text");
    lua_pushcfunction(L, display_text_draw);
    lua_setfield(L, -2, "draw_text");
    lua_pushcfunction(L, display_text_draw_aligned);
    lua_setfield(L, -2, "draw_text_aligned");
}
