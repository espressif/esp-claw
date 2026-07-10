/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lua "display" module.
 *
 * Ported from esp-claw lua_module_halo_display.c.
 * HAL calls are routed through display_hal.h which the board layer must
 * implement. Image file loading and decoding live in lua_module_image.
 */
#include "lua_module_display.h"

#include "cap_lua.h"
#include "display_color.h"
#include "display_arbiter.h"
#include "display_hal.h"
#include "display_text.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "lua_image.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "lua_display";

/* -------------------------------------------------------------------------
 * Argument helpers (mirrors the reference implementation)
 * ---------------------------------------------------------------------- */

static int lua_display_check_integer_arg(lua_State *L, int index, const char *name)
{
    if (!lua_isinteger(L, index)) {
        return luaL_error(L, "display %s must be an integer", name);
    }
    return (int)lua_tointeger(L, index);
}

static float lua_display_check_number_arg(lua_State *L, int index, const char *name)
{
    if (!lua_isnumber(L, index)) {
        luaL_error(L, "display %s must be a number", name);
        return 0.0f;
    }
    return (float)lua_tonumber(L, index);
}

static void *lua_display_check_lightuserdata_arg(lua_State *L, int index, const char *name)
{
    void *ptr = lua_touserdata(L, index);

    luaL_argcheck(L, ptr != NULL, index, name);
    return ptr;
}

static const uint8_t *lua_display_check_buffer_arg(lua_State *L, int index, size_t expected,
                                                   size_t *out_len)
{
    if (lua_islightuserdata(L, index)) {
        const void *ptr = lua_touserdata(L, index);
        luaL_argcheck(L, ptr != NULL, index, "display buffer lightuserdata expected");
        if (out_len != NULL) {
            *out_len = expected;
        }
        return (const uint8_t *)ptr;
    }

    size_t data_len = 0;
    const uint8_t *data = (const uint8_t *)luaL_checklstring(L, index, &data_len);
    if (out_len != NULL) {
        *out_len = data_len;
    }
    return data;
}

static int lua_display_checked_rgb565_bytes(lua_State *L, int width, int height, const char *api, size_t *out_bytes)
{
    size_t pixels;

    if (out_bytes == NULL) {
        return luaL_error(L, "%s: internal size output missing", api);
    }
    *out_bytes = 0;
    if (width <= 0 || height <= 0 || (size_t)width > SIZE_MAX / (size_t)height) {
        return luaL_error(L, "%s: invalid size (%d x %d)", api, width, height);
    }
    pixels = (size_t)width * (size_t)height;
    if (pixels > SIZE_MAX / 2) {
        return luaL_error(L, "%s: buffer size overflow (%d x %d)", api, width, height);
    }
    *out_bytes = pixels * 2;
    return 0;
}

static void *lua_display_opt_lightuserdata_arg(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) {
        return NULL;
    }

    return lua_display_check_lightuserdata_arg(L, index,
                                               "display io_handle lightuserdata expected");
}

static display_hal_panel_if_t lua_display_parse_panel_if(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) {
        return DISPLAY_HAL_PANEL_IF_IO;
    }

    if (!lua_isinteger(L, index)) {
        luaL_error(L, "display panel_if must be an interface constant");
        return DISPLAY_HAL_PANEL_IF_IO;
    }

    lua_Integer value = lua_tointeger(L, index);

    if (value >= DISPLAY_HAL_PANEL_IF_IO && value <= DISPLAY_HAL_PANEL_IF_MIPI_DSI) {
        return (display_hal_panel_if_t)value;
    }

    luaL_error(L, "display panel_if integer is out of range");
    return DISPLAY_HAL_PANEL_IF_IO;
}

static display_color_t lua_display_check_color_arg(lua_State *L, int index, const char *name)
{
    display_color_t color = {0};
    esp_err_t err = display_color_from_lua(L, index, &color);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s color parse failed: %s", name, esp_err_to_name(err));
        luaL_error(L, "display %s color invalid: %s", name, esp_err_to_name(err));
    }
    return color;
}

static void lua_display_reject_table_field(lua_State *L, int index, const char *field, const char *context)
{
    lua_getfield(L, index, field);
    bool exists = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (exists) {
        luaL_error(L, "display %s no longer supports '%s'; use a single color value", context, field);
    }
}

/* -------------------------------------------------------------------------
 * Screen lifecycle
 * ---------------------------------------------------------------------- */

static void lua_display_exit_cleanup(lua_State *L)
{
    (void)L;

    if (!display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_LUA)) {
        return;
    }
    ESP_LOGI(TAG, "Lua exit cleanup: display still owned by Lua, releasing");

    if (display_hal_destroy() == ESP_OK) {
        display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
    }
}

static int lua_display_init(lua_State *L)
{
    esp_lcd_panel_handle_t panel_handle =
        (esp_lcd_panel_handle_t)lua_display_check_lightuserdata_arg(
            L, 1, "display panel_handle lightuserdata expected");
    esp_lcd_panel_io_handle_t io_handle =
        (esp_lcd_panel_io_handle_t)lua_display_opt_lightuserdata_arg(L, 2);
    int lcd_width = lua_display_check_integer_arg(L, 3, "lcd_width");
    int lcd_height = lua_display_check_integer_arg(L, 4, "lcd_height");
    display_hal_panel_if_t panel_if = lua_display_parse_panel_if(L, 5);

    esp_err_t err = display_arbiter_acquire(DISPLAY_ARBITER_OWNER_LUA);
    if (err != ESP_OK) {
        return luaL_error(L, "display init acquire failed: %s", esp_err_to_name(err));
    }

    err = display_hal_create(panel_handle, io_handle, panel_if, lcd_width, lcd_height);
    if (err != ESP_OK) {
        display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
        return luaL_error(L, "display init failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_display_deinit(lua_State *L)
{
    (void)L;
    esp_err_t err = display_hal_destroy();
    if (err != ESP_OK) {
        return luaL_error(L, "display deinit failed: %s", esp_err_to_name(err));
    }

    err = display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
    if (err != ESP_OK) {
        return luaL_error(L, "display release failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* -------------------------------------------------------------------------
 * Basic drawing
 * ---------------------------------------------------------------------- */

static int lua_display_clear(lua_State *L)
{
    display_color_t color = lua_display_check_color_arg(L, 1, "argument");
    esp_err_t err = display_hal_clear(color);
    if (err != ESP_OK) {
        return luaL_error(L, "display clear failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_set_clip_rect(lua_State *L)
{
    int x      = lua_display_check_integer_arg(L, 1, "x");
    int y      = lua_display_check_integer_arg(L, 2, "y");
    int width  = lua_display_check_integer_arg(L, 3, "width");
    int height = lua_display_check_integer_arg(L, 4, "height");
    esp_err_t err = display_hal_set_clip_rect(x, y, width, height);
    if (err != ESP_OK) {
        return luaL_error(L, "display set_clip_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_clear_clip_rect(lua_State *L)
{
    (void)L;
    esp_err_t err = display_hal_clear_clip_rect();
    if (err != ESP_OK) {
        return luaL_error(L, "display clear_clip_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_fill_rect(lua_State *L)
{
    int x      = lua_display_check_integer_arg(L, 1, "x");
    int y      = lua_display_check_integer_arg(L, 2, "y");
    int width  = lua_display_check_integer_arg(L, 3, "width");
    int height = lua_display_check_integer_arg(L, 4, "height");
    display_color_t color = lua_display_check_color_arg(L, 5, "argument");
    esp_err_t err = display_hal_fill_rect(x, y, width, height, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display fill_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_pixel(lua_State *L)
{
    int x = lua_display_check_integer_arg(L, 1, "x");
    int y = lua_display_check_integer_arg(L, 2, "y");
    display_color_t color = lua_display_check_color_arg(L, 3, "argument");
    esp_err_t err = display_hal_draw_pixel(x, y, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_pixel failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_line(lua_State *L)
{
    int x0 = lua_display_check_integer_arg(L, 1, "x0");
    int y0 = lua_display_check_integer_arg(L, 2, "y0");
    int x1 = lua_display_check_integer_arg(L, 3, "x1");
    int y1 = lua_display_check_integer_arg(L, 4, "y1");
    display_color_t color = lua_display_check_color_arg(L, 5, "argument");
    esp_err_t err = display_hal_draw_line(x0, y0, x1, y1, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_line failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_rect(lua_State *L)
{
    int x      = lua_display_check_integer_arg(L, 1, "x");
    int y      = lua_display_check_integer_arg(L, 2, "y");
    int width  = lua_display_check_integer_arg(L, 3, "width");
    int height = lua_display_check_integer_arg(L, 4, "height");
    display_color_t color = lua_display_check_color_arg(L, 5, "argument");
    esp_err_t err = display_hal_draw_rect(x, y, width, height, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_backlight(lua_State *L)
{
    int on = lua_toboolean(L, 1);
    esp_err_t err = display_hal_set_backlight(on != 0);
    if (err != ESP_OK) {
        return luaL_error(L, "display backlight failed: %s", esp_err_to_name(err));
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Frame management
 * ---------------------------------------------------------------------- */

static void lua_display_parse_frame_options(lua_State *L, int index,
                                            bool *clear, display_color_t *color)
{
    if (clear)  { *clear = true; }
    if (color)  { *color = (display_color_t){ 0, 0, 0, 255 }; }

    if (lua_isnoneornil(L, index)) {
        return;
    }
    luaL_checktype(L, index, LUA_TTABLE);
    lua_display_reject_table_field(L, index, "r", "frame options");
    lua_display_reject_table_field(L, index, "g", "frame options");
    lua_display_reject_table_field(L, index, "b", "frame options");

    lua_getfield(L, index, "clear");
    if (!lua_isnil(L, -1) && clear) {
        *clear = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "color");
    if (!lua_isnil(L, -1) && color) {
        esp_err_t err = display_color_from_lua(L, -1, color);
        if (err != ESP_OK) {
            luaL_error(L, "display frame color invalid: %s", esp_err_to_name(err));
        }
    }
    lua_pop(L, 1);
}

static int lua_display_begin_frame(lua_State *L)
{
    bool clear = true;
    display_color_t color = { 0, 0, 0, 255 };
    lua_display_parse_frame_options(L, 1, &clear, &color);
    esp_err_t err = display_hal_begin_frame(clear, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display begin_frame failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_present(lua_State *L)
{
    (void)L;
    esp_err_t err = display_hal_present();
    if (err != ESP_OK) {
        return luaL_error(L, "display present failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_present_full(lua_State *L)
{
    (void)L;
    esp_err_t err = display_hal_present_full();
    if (err != ESP_OK) {
        return luaL_error(L, "display present_full failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_end_frame(lua_State *L)
{
    (void)L;
    esp_err_t err = display_hal_end_frame();
    if (err != ESP_OK) {
        return luaL_error(L, "display end_frame failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_frame_active(lua_State *L)
{
    lua_pushboolean(L, display_hal_is_frame_active());
    return 1;
}

static int lua_display_animation_info(lua_State *L)
{
    display_hal_animation_info_t info = {0};
    esp_err_t err = display_hal_get_animation_info(&info);
    if (err != ESP_OK) {
        return luaL_error(L, "display animation_info failed: %s", esp_err_to_name(err));
    }
    lua_newtable(L);
    lua_pushinteger(L, info.framebuffer_count);
    lua_setfield(L, -2, "framebuffer_count");
    lua_pushboolean(L, info.double_buffered);
    lua_setfield(L, -2, "double_buffered");
    lua_pushboolean(L, info.frame_active);
    lua_setfield(L, -2, "frame_active");
    lua_pushboolean(L, info.flush_in_flight);
    lua_setfield(L, -2, "flush_in_flight");
    return 1;
}

static int lua_display_module_index(lua_State *L)
{
    const char *key = luaL_checkstring(L, 2);
    if (strcmp(key, "width") == 0) {
        lua_pushinteger(L, display_hal_width());
        return 1;
    }
    if (strcmp(key, "height") == 0) {
        lua_pushinteger(L, display_hal_height());
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int lua_display_module_newindex(lua_State *L)
{
    const char *key = luaL_checkstring(L, 2);
    if (strcmp(key, "width") == 0 || strcmp(key, "height") == 0) {
        return luaL_error(L, "display.%s is read-only", key);
    }
    lua_rawset(L, 1);
    return 0;
}

static void lua_display_set_module_metatable(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_display_module_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, lua_display_module_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
}

/* -------------------------------------------------------------------------
 * Pixel buffers and images
 * ---------------------------------------------------------------------- */

static int lua_display_align_down(int value, int align)
{
    return value - (value % align);
}

typedef enum {
    LUA_DISPLAY_IMAGE_RAW,
    LUA_DISPLAY_IMAGE_FIT,
    LUA_DISPLAY_IMAGE_COVER,
    LUA_DISPLAY_IMAGE_STRETCH,
    LUA_DISPLAY_IMAGE_CROP,
} lua_display_image_mode_t;

typedef struct {
    lua_display_image_mode_t mode;
    int dst_w;
    int dst_h;
    int full_w;
    int full_h;
    int src_x;
    int src_y;
    int src_w;
    int src_h;
} lua_display_image_options_t;

static bool lua_display_get_table_integer(lua_State *L, int table_idx, const char *name, int *out)
{
    bool ok = false;

    lua_getfield(L, table_idx, name);
    if (lua_isinteger(L, -1)) {
        *out = (int)lua_tointeger(L, -1);
        ok = true;
    } else if (lua_isnumber(L, -1)) {
        *out = (int)lua_tonumber(L, -1);
        ok = true;
    }
    lua_pop(L, 1);
    return ok;
}

static lua_display_image_mode_t lua_display_parse_image_mode(lua_State *L, int opts_idx, const char *context)
{
    const char *mode = NULL;

    if (lua_isnoneornil(L, opts_idx)) {
        return LUA_DISPLAY_IMAGE_RAW;
    }
    lua_getfield(L, opts_idx, "mode");
    mode = lua_isnil(L, -1) ? "raw" : luaL_checkstring(L, -1);
    if (strcmp(mode, "raw") == 0) {
        lua_pop(L, 1);
        return LUA_DISPLAY_IMAGE_RAW;
    }
    if (strcmp(mode, "fit") == 0) {
        lua_pop(L, 1);
        return LUA_DISPLAY_IMAGE_FIT;
    }
    if (strcmp(mode, "cover") == 0) {
        lua_pop(L, 1);
        return LUA_DISPLAY_IMAGE_COVER;
    }
    if (strcmp(mode, "stretch") == 0) {
        lua_pop(L, 1);
        return LUA_DISPLAY_IMAGE_STRETCH;
    }
    if (strcmp(mode, "crop") == 0) {
        lua_pop(L, 1);
        return LUA_DISPLAY_IMAGE_CROP;
    }
    luaL_error(L, "display %s unsupported mode: %s", context, mode);
    lua_pop(L, 1);
    return LUA_DISPLAY_IMAGE_RAW;
}

static void lua_display_parse_source_rect(lua_State *L, int opts_idx, lua_display_image_options_t *opts)
{
    lua_getfield(L, opts_idx, "source");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    luaL_checktype(L, -1, LUA_TTABLE);
    lua_display_get_table_integer(L, -1, "x", &opts->src_x);
    lua_display_get_table_integer(L, -1, "y", &opts->src_y);
    lua_display_get_table_integer(L, -1, "width", &opts->src_w);
    lua_display_get_table_integer(L, -1, "height", &opts->src_h);
    lua_pop(L, 1);
}

static void lua_display_parse_image_options(lua_State *L, int opts_idx, int src_w, int src_h, lua_display_image_options_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->mode = LUA_DISPLAY_IMAGE_RAW;
    opts->dst_w = src_w;
    opts->dst_h = src_h;
    opts->full_w = src_w;
    opts->full_h = src_h;
    opts->src_w = src_w;
    opts->src_h = src_h;

    if (lua_isnoneornil(L, opts_idx)) {
        return;
    }
    luaL_checktype(L, opts_idx, LUA_TTABLE);
    opts->mode = lua_display_parse_image_mode(L, opts_idx, "draw_image");
    lua_display_get_table_integer(L, opts_idx, "width", &opts->dst_w);
    lua_display_get_table_integer(L, opts_idx, "height", &opts->dst_h);
    lua_display_parse_source_rect(L, opts_idx, opts);
}

static esp_err_t lua_display_draw_pixels_fit_data(int x, int y, int src_width, int src_height,
                                                  int max_w, int max_h, const uint16_t *data, int *out_w, int *out_h)
{
    if (src_width <= max_w && src_height <= max_h) {
        esp_err_t err = display_hal_draw_bitmap(x, y, src_width, src_height, data);
        if (err != ESP_OK) {
            return err;
        }
        if (out_w) {
            *out_w = src_width;
        }
        if (out_h) {
            *out_h = src_height;
        }
        return ESP_OK;
    }

    double ratio_w = (double)max_w / src_width;
    double ratio_h = (double)max_h / src_height;
    double ratio = (ratio_w < ratio_h) ? ratio_w : ratio_h;
    int scale_w = (int)(src_width * ratio);
    int scale_h = (int)(src_height * ratio);

    if (scale_w <= 0) {
        scale_w = 1;
    }
    if (scale_h <= 0) {
        scale_h = 1;
    }
    if (scale_w >= 8) {
        scale_w = lua_display_align_down(scale_w, 8);
        if (scale_w == 0) {
            scale_w = 8;
        }
    }
    if (scale_h >= 8) {
        scale_h = lua_display_align_down(scale_h, 8);
        if (scale_h == 0) {
            scale_h = 8;
        }
    }
    return display_hal_draw_bitmap_scaled(x, y, data, src_width, src_height, scale_w, scale_h, out_w, out_h);
}

static esp_err_t lua_display_copy_rgb565_crop(const uint16_t *src, int src_width, int src_x, int src_y, int crop_w, int crop_h, uint16_t **out)
{
    uint16_t *crop = NULL;

    if (out == NULL || src == NULL || src_width <= 0 || crop_w <= 0 || crop_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    crop = (uint16_t *)malloc((size_t)crop_w * (size_t)crop_h * sizeof(uint16_t));
    if (crop == NULL) {
        ESP_LOGE(TAG, "display crop buffer alloc failed: %dx%d", crop_w, crop_h);
        return ESP_ERR_NO_MEM;
    }
    for (int row = 0; row < crop_h; row++) {
        const uint16_t *src_row = src + ((size_t)(src_y + row) * src_width) + src_x;
        memcpy(crop + ((size_t)row * crop_w), src_row, (size_t)crop_w * sizeof(uint16_t));
    }
    *out = crop;
    return ESP_OK;
}

static int lua_display_draw_image(lua_State *L)
{
    int x = lua_display_check_integer_arg(L, 1, "x");
    int y = lua_display_check_integer_arg(L, 2, "y");
    lua_image_view_t view = {0};
    lua_display_image_options_t opts = {0};
    const uint16_t *pixels = NULL;
    int out_w = 0;
    int out_h = 0;
    esp_err_t err = lua_image_require_format(L, 3, LUA_IMAGE_FORMAT_RGB565LE, &view);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display image require RGB565 failed: %s", esp_err_to_name(err));
        return luaL_error(L, "display draw_image image format failed: %s", esp_err_to_name(err));
    }

    lua_display_parse_image_options(L, 4, view.width, view.height, &opts);
    pixels = (const uint16_t *)view.data;
    if (opts.dst_w <= 0 || opts.dst_h <= 0 || opts.src_w <= 0 || opts.src_h <= 0) {
        lua_image_release_view(&view);
        return luaL_error(L, "display draw_image invalid image size");
    }
    if (opts.src_x < 0 || opts.src_y < 0 || opts.src_x + opts.src_w > view.width || opts.src_y + opts.src_h > view.height) {
        lua_image_release_view(&view);
        return luaL_error(L, "display draw_image source rectangle out of bounds");
    }

    switch (opts.mode) {
    case LUA_DISPLAY_IMAGE_RAW:
        err = display_hal_draw_bitmap(x, y, view.width, view.height, pixels);
        out_w = view.width;
        out_h = view.height;
        break;
    case LUA_DISPLAY_IMAGE_FIT:
        err = lua_display_draw_pixels_fit_data(x, y, view.width, view.height, opts.dst_w, opts.dst_h, pixels, &out_w, &out_h);
        break;
    case LUA_DISPLAY_IMAGE_STRETCH:
        err = display_hal_draw_bitmap_scaled(x, y, pixels, view.width, view.height, opts.dst_w, opts.dst_h, &out_w, &out_h);
        break;
    case LUA_DISPLAY_IMAGE_CROP:
    case LUA_DISPLAY_IMAGE_COVER: {
        uint16_t *crop = NULL;
        int src_x = opts.src_x;
        int src_y = opts.src_y;
        int src_w = opts.src_w;
        int src_h = opts.src_h;

        if (opts.mode == LUA_DISPLAY_IMAGE_COVER) {
            int64_t lhs = (int64_t)src_w * opts.dst_h;
            int64_t rhs = (int64_t)opts.dst_w * src_h;
            if (lhs > rhs) {
                int new_w = (int)((int64_t)src_h * opts.dst_w / opts.dst_h);
                src_x += (src_w - new_w) / 2;
                src_w = new_w;
            } else if (lhs < rhs) {
                int new_h = (int)((int64_t)src_w * opts.dst_h / opts.dst_w);
                src_y += (src_h - new_h) / 2;
                src_h = new_h;
            }
        }
        if (src_w == opts.dst_w && src_h == opts.dst_h) {
            err = display_hal_draw_bitmap_crop(x, y, src_x, src_y, src_w, src_h, view.width, view.height, pixels);
            out_w = src_w;
            out_h = src_h;
            break;
        }
        err = lua_display_copy_rgb565_crop(pixels, view.width, src_x, src_y, src_w, src_h, &crop);
        if (err == ESP_OK) {
            err = display_hal_draw_bitmap_scaled(x, y, crop, src_w, src_h, opts.dst_w, opts.dst_h, &out_w, &out_h);
        }
        free(crop);
        break;
    }
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    lua_image_release_view(&view);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display draw_image failed: %s", esp_err_to_name(err));
        return luaL_error(L, "display draw_image failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, out_w);
    lua_pushinteger(L, out_h);
    return 2;
}

static bool lua_display_get_pixels_dest_size(lua_State *L, int opts_idx, lua_display_image_options_t *opts)
{
    bool has_w = lua_display_get_table_integer(L, opts_idx, "dst_width", &opts->dst_w);
    bool has_h = lua_display_get_table_integer(L, opts_idx, "dst_height", &opts->dst_h);

    if (!has_w) {
        has_w = lua_display_get_table_integer(L, opts_idx, "max_width", &opts->dst_w);
    }
    if (!has_h) {
        has_h = lua_display_get_table_integer(L, opts_idx, "max_height", &opts->dst_h);
    }
    return has_w && has_h;
}

static bool lua_display_pixels_source_is_full(const lua_display_image_options_t *opts)
{
    return opts->src_x == 0 && opts->src_y == 0 && opts->src_w == opts->full_w && opts->src_h == opts->full_h;
}

static void lua_display_parse_pixels_options(lua_State *L, int opts_idx, lua_display_image_options_t *opts)
{
    bool has_dest = false;

    memset(opts, 0, sizeof(*opts));
    opts->mode = LUA_DISPLAY_IMAGE_RAW;

    if (lua_isnoneornil(L, opts_idx)) {
        luaL_error(L, "display draw_pixels options table required");
    }
    luaL_checktype(L, opts_idx, LUA_TTABLE);
    opts->mode = lua_display_parse_image_mode(L, opts_idx, "draw_pixels");
    lua_display_get_table_integer(L, opts_idx, "width", &opts->src_w);
    lua_display_get_table_integer(L, opts_idx, "height", &opts->src_h);
    opts->full_w = opts->src_w;
    opts->full_h = opts->src_h;
    opts->dst_w = opts->src_w;
    opts->dst_h = opts->src_h;
    has_dest = lua_display_get_pixels_dest_size(L, opts_idx, opts);
    lua_display_parse_source_rect(L, opts_idx, opts);
    if (!has_dest) {
        opts->dst_w = opts->src_w;
        opts->dst_h = opts->src_h;
    }

    lua_getfield(L, opts_idx, "format");
    if (!lua_isnil(L, -1)) {
        const char *format = luaL_checkstring(L, -1);
        if (strcmp(format, "rgb565") != 0 && strcmp(format, "rgb565le") != 0) {
            luaL_error(L, "display draw_pixels format must be rgb565");
        }
    }
    lua_pop(L, 1);
}

static esp_err_t lua_display_draw_pixels_data(int x, int y, const uint16_t *pixels,
                                              const lua_display_image_options_t *opts, int *out_w, int *out_h)
{
    esp_err_t err = ESP_OK;

    switch (opts->mode) {
    case LUA_DISPLAY_IMAGE_RAW:
        if (!lua_display_pixels_source_is_full(opts)) {
            err = display_hal_draw_bitmap_crop(x, y, opts->src_x, opts->src_y, opts->src_w, opts->src_h, opts->full_w, opts->full_h, pixels);
        } else {
            err = display_hal_draw_bitmap(x, y, opts->full_w, opts->full_h, pixels);
        }
        if (out_w) {
            *out_w = opts->src_w;
        }
        if (out_h) {
            *out_h = opts->src_h;
        }
        break;
    case LUA_DISPLAY_IMAGE_FIT: {
        uint16_t *crop = NULL;
        const uint16_t *src_pixels = pixels;

        if (!lua_display_pixels_source_is_full(opts)) {
            err = lua_display_copy_rgb565_crop(pixels, opts->full_w, opts->src_x, opts->src_y, opts->src_w, opts->src_h, &crop);
            if (err != ESP_OK) {
                break;
            }
            src_pixels = crop;
        }
        err = lua_display_draw_pixels_fit_data(x, y, opts->src_w, opts->src_h, opts->dst_w, opts->dst_h, src_pixels, out_w, out_h);
        free(crop);
        break;
    }
    case LUA_DISPLAY_IMAGE_STRETCH: {
        uint16_t *crop = NULL;
        const uint16_t *src_pixels = pixels;

        if (!lua_display_pixels_source_is_full(opts)) {
            err = lua_display_copy_rgb565_crop(pixels, opts->full_w, opts->src_x, opts->src_y, opts->src_w, opts->src_h, &crop);
            if (err != ESP_OK) {
                break;
            }
            src_pixels = crop;
        }
        err = display_hal_draw_bitmap_scaled(x, y, src_pixels, opts->src_w, opts->src_h, opts->dst_w, opts->dst_h, out_w, out_h);
        free(crop);
        break;
    }
    case LUA_DISPLAY_IMAGE_CROP:
    case LUA_DISPLAY_IMAGE_COVER: {
        uint16_t *crop = NULL;
        int src_x = opts->src_x;
        int src_y = opts->src_y;
        int src_w = opts->src_w;
        int src_h = opts->src_h;
        int dst_w = opts->dst_w;
        int dst_h = opts->dst_h;

        if (opts->mode == LUA_DISPLAY_IMAGE_COVER) {
            int64_t lhs = (int64_t)src_w * dst_h;
            int64_t rhs = (int64_t)dst_w * src_h;
            if (lhs > rhs) {
                int new_w = (int)((int64_t)src_h * dst_w / dst_h);
                src_x += (src_w - new_w) / 2;
                src_w = new_w;
            } else if (lhs < rhs) {
                int new_h = (int)((int64_t)src_w * dst_h / dst_w);
                src_y += (src_h - new_h) / 2;
                src_h = new_h;
            }
        }
        if (src_w == dst_w && src_h == dst_h) {
            err = display_hal_draw_bitmap_crop(x, y, src_x, src_y, src_w, src_h, opts->full_w, opts->full_h, pixels);
            if (out_w) {
                *out_w = src_w;
            }
            if (out_h) {
                *out_h = src_h;
            }
            break;
        }
        err = lua_display_copy_rgb565_crop(pixels, opts->full_w, src_x, src_y, src_w, src_h, &crop);
        if (err == ESP_OK) {
            err = display_hal_draw_bitmap_scaled(x, y, crop, src_w, src_h, dst_w, dst_h, out_w, out_h);
        }
        free(crop);
        break;
    }
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }
    return err;
}

static int lua_display_draw_pixels(lua_State *L)
{
    int x = lua_display_check_integer_arg(L, 1, "x");
    int y = lua_display_check_integer_arg(L, 2, "y");
    lua_display_image_options_t opts = {0};
    size_t expected = 0;
    size_t data_len = 0;
    int out_w = 0;
    int out_h = 0;

    lua_display_parse_pixels_options(L, 4, &opts);
    if (opts.src_w <= 0 || opts.src_h <= 0 || opts.dst_w <= 0 || opts.dst_h <= 0) {
        return luaL_error(L, "display draw_pixels invalid image size");
    }
    if (opts.src_x < 0 || opts.src_y < 0 || opts.src_x + opts.src_w > opts.full_w || opts.src_y + opts.src_h > opts.full_h) {
        return luaL_error(L, "display draw_pixels source rectangle out of bounds");
    }
    lua_display_checked_rgb565_bytes(L, opts.full_w, opts.full_h, "draw_pixels", &expected);
    const uint8_t *data = lua_display_check_buffer_arg(L, 3, expected, &data_len);
    if (data_len < expected) {
        return luaL_error(L, "draw_pixels: data too short (%d bytes, need %d)", (int)data_len, (int)expected);
    }

    esp_err_t err = lua_display_draw_pixels_data(x, y, (const uint16_t *)data, &opts, &out_w, &out_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display draw_pixels failed: %s", esp_err_to_name(err));
        return luaL_error(L, "display draw_pixels failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, out_w);
    lua_pushinteger(L, out_h);
    return 2;
}

/* -------------------------------------------------------------------------
 * Shape drawing
 * ---------------------------------------------------------------------- */

static int lua_display_fill_circle(lua_State *L)
{
    int cx = lua_display_check_integer_arg(L, 1, "cx");
    int cy = lua_display_check_integer_arg(L, 2, "cy");
    int r  = lua_display_check_integer_arg(L, 3, "radius");
    display_color_t color = lua_display_check_color_arg(L, 4, "argument");
    esp_err_t err = display_hal_fill_circle(cx, cy, r, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display fill_circle failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_circle(lua_State *L)
{
    int cx = lua_display_check_integer_arg(L, 1, "cx");
    int cy = lua_display_check_integer_arg(L, 2, "cy");
    int r  = lua_display_check_integer_arg(L, 3, "radius");
    display_color_t color = lua_display_check_color_arg(L, 4, "argument");
    esp_err_t err = display_hal_draw_circle(cx, cy, r, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_circle failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_arc(lua_State *L)
{
    int cx     = lua_display_check_integer_arg(L, 1, "cx");
    int cy     = lua_display_check_integer_arg(L, 2, "cy");
    int radius = lua_display_check_integer_arg(L, 3, "radius");
    float start_deg = lua_display_check_number_arg(L, 4, "start_deg");
    float end_deg   = lua_display_check_number_arg(L, 5, "end_deg");
    display_color_t color = lua_display_check_color_arg(L, 6, "argument");
    esp_err_t err = display_hal_draw_arc(cx, cy, radius, start_deg, end_deg, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_arc failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_fill_arc(lua_State *L)
{
    int cx           = lua_display_check_integer_arg(L, 1, "cx");
    int cy           = lua_display_check_integer_arg(L, 2, "cy");
    int inner_radius = lua_display_check_integer_arg(L, 3, "inner_radius");
    int outer_radius = lua_display_check_integer_arg(L, 4, "outer_radius");
    float start_deg  = lua_display_check_number_arg(L, 5, "start_deg");
    float end_deg    = lua_display_check_number_arg(L, 6, "end_deg");
    display_color_t color = lua_display_check_color_arg(L, 7, "argument");
    esp_err_t err = display_hal_fill_arc(cx, cy, inner_radius, outer_radius,
                                         start_deg, end_deg, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display fill_arc failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_ellipse(lua_State *L)
{
    int cx       = lua_display_check_integer_arg(L, 1, "cx");
    int cy       = lua_display_check_integer_arg(L, 2, "cy");
    int radius_x = lua_display_check_integer_arg(L, 3, "radius_x");
    int radius_y = lua_display_check_integer_arg(L, 4, "radius_y");
    display_color_t color = lua_display_check_color_arg(L, 5, "argument");
    esp_err_t err = display_hal_draw_ellipse(cx, cy, radius_x, radius_y, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_ellipse failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_fill_ellipse(lua_State *L)
{
    int cx       = lua_display_check_integer_arg(L, 1, "cx");
    int cy       = lua_display_check_integer_arg(L, 2, "cy");
    int radius_x = lua_display_check_integer_arg(L, 3, "radius_x");
    int radius_y = lua_display_check_integer_arg(L, 4, "radius_y");
    display_color_t color = lua_display_check_color_arg(L, 5, "argument");
    esp_err_t err = display_hal_fill_ellipse(cx, cy, radius_x, radius_y, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display fill_ellipse failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_round_rect(lua_State *L)
{
    int x      = lua_display_check_integer_arg(L, 1, "x");
    int y      = lua_display_check_integer_arg(L, 2, "y");
    int width  = lua_display_check_integer_arg(L, 3, "width");
    int height = lua_display_check_integer_arg(L, 4, "height");
    int radius = lua_display_check_integer_arg(L, 5, "radius");
    display_color_t color = lua_display_check_color_arg(L, 6, "argument");
    esp_err_t err = display_hal_draw_round_rect(x, y, width, height, radius, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_round_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_fill_round_rect(lua_State *L)
{
    int x      = lua_display_check_integer_arg(L, 1, "x");
    int y      = lua_display_check_integer_arg(L, 2, "y");
    int width  = lua_display_check_integer_arg(L, 3, "width");
    int height = lua_display_check_integer_arg(L, 4, "height");
    int radius = lua_display_check_integer_arg(L, 5, "radius");
    display_color_t color = lua_display_check_color_arg(L, 6, "argument");
    esp_err_t err = display_hal_fill_round_rect(x, y, width, height, radius, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display fill_round_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_triangle(lua_State *L)
{
    int x1 = lua_display_check_integer_arg(L, 1, "x1");
    int y1 = lua_display_check_integer_arg(L, 2, "y1");
    int x2 = lua_display_check_integer_arg(L, 3, "x2");
    int y2 = lua_display_check_integer_arg(L, 4, "y2");
    int x3 = lua_display_check_integer_arg(L, 5, "x3");
    int y3 = lua_display_check_integer_arg(L, 6, "y3");
    display_color_t color = lua_display_check_color_arg(L, 7, "argument");
    esp_err_t err = display_hal_draw_triangle(x1, y1, x2, y2, x3, y3, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_triangle failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_fill_triangle(lua_State *L)
{
    int x1 = lua_display_check_integer_arg(L, 1, "x1");
    int y1 = lua_display_check_integer_arg(L, 2, "y1");
    int x2 = lua_display_check_integer_arg(L, 3, "x2");
    int y2 = lua_display_check_integer_arg(L, 4, "y2");
    int x3 = lua_display_check_integer_arg(L, 5, "x3");
    int y3 = lua_display_check_integer_arg(L, 6, "y3");
    display_color_t color = lua_display_check_color_arg(L, 7, "argument");
    esp_err_t err = display_hal_fill_triangle(x1, y1, x2, y2, x3, y3, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display fill_triangle failed: %s", esp_err_to_name(err));
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Module registration
 * ---------------------------------------------------------------------- */

int luaopen_display(lua_State *L)
{
    lua_newtable(L);

    lua_pushcfunction(L, lua_display_init);
    lua_setfield(L, -2, "init");
    lua_pushcfunction(L, lua_display_deinit);
    lua_setfield(L, -2, "deinit");

    lua_pushcfunction(L, lua_display_clear);
    lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, lua_display_set_clip_rect);
    lua_setfield(L, -2, "set_clip_rect");
    lua_pushcfunction(L, lua_display_clear_clip_rect);
    lua_setfield(L, -2, "clear_clip_rect");

    lua_pushcfunction(L, lua_display_fill_rect);
    lua_setfield(L, -2, "fill_rect");
    lua_pushcfunction(L, lua_display_draw_rect);
    lua_setfield(L, -2, "draw_rect");
    lua_pushcfunction(L, lua_display_draw_pixel);
    lua_setfield(L, -2, "draw_pixel");
    lua_pushcfunction(L, lua_display_draw_line);
    lua_setfield(L, -2, "draw_line");

    lua_pushcfunction(L, lua_display_backlight);
    lua_setfield(L, -2, "backlight");

    lua_pushcfunction(L, lua_display_begin_frame);
    lua_setfield(L, -2, "begin_frame");
    lua_pushcfunction(L, lua_display_present);
    lua_setfield(L, -2, "present");
    lua_pushcfunction(L, lua_display_present_full);
    lua_setfield(L, -2, "present_full");
    lua_pushcfunction(L, lua_display_end_frame);
    lua_setfield(L, -2, "end_frame");
    lua_pushcfunction(L, lua_display_frame_active);
    lua_setfield(L, -2, "frame_active");
    lua_pushcfunction(L, lua_display_animation_info);
    lua_setfield(L, -2, "animation_info");

    display_text_register_lua(L);

    lua_pushcfunction(L, lua_display_draw_image);
    lua_setfield(L, -2, "draw_image");
    lua_pushcfunction(L, lua_display_draw_pixels);
    lua_setfield(L, -2, "draw_pixels");

    lua_pushcfunction(L, lua_display_fill_circle);
    lua_setfield(L, -2, "fill_circle");
    lua_pushcfunction(L, lua_display_draw_circle);
    lua_setfield(L, -2, "draw_circle");
    lua_pushcfunction(L, lua_display_draw_arc);
    lua_setfield(L, -2, "draw_arc");
    lua_pushcfunction(L, lua_display_fill_arc);
    lua_setfield(L, -2, "fill_arc");

    lua_pushcfunction(L, lua_display_draw_ellipse);
    lua_setfield(L, -2, "draw_ellipse");
    lua_pushcfunction(L, lua_display_fill_ellipse);
    lua_setfield(L, -2, "fill_ellipse");

    lua_pushcfunction(L, lua_display_draw_round_rect);
    lua_setfield(L, -2, "draw_round_rect");
    lua_pushcfunction(L, lua_display_fill_round_rect);
    lua_setfield(L, -2, "fill_round_rect");

    lua_pushcfunction(L, lua_display_draw_triangle);
    lua_setfield(L, -2, "draw_triangle");
    lua_pushcfunction(L, lua_display_fill_triangle);
    lua_setfield(L, -2, "fill_triangle");

    lua_display_set_module_metatable(L);
    return 1;
}

esp_err_t lua_module_display_register(void)
{
    esp_err_t err = cap_lua_register_module("display", luaopen_display);
    if (err != ESP_OK) {
        return err;
    }
    return cap_lua_register_exit_cleanup(lua_display_exit_cleanup);
}
