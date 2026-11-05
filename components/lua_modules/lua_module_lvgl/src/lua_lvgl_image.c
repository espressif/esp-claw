/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"

static int image_set_raw(lua_State *L, lv_color_format_t format)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lua_Integer width = luaL_checkinteger(L, 2);
    lua_Integer height = luaL_checkinteger(L, 3);
    size_t length;
    const uint8_t *pixels = (const uint8_t *)luaL_checklstring(L, 4, &length);
    size_t pixel_size = format == LV_COLOR_FORMAT_RGB565 ? 2 : 4;
    bool swapped = false;
    if (!lua_isnoneornil(L, 5)) {
        luaL_checktype(L, 5, LUA_TTABLE);
        swapped = lua_lvgl_get_opt_bool_field(L, 5, "swapped", false);
    }
    luaL_argcheck(L, !swapped || pixel_size == 2, 5, "swapped only applies to RGB565");
    luaL_argcheck(L, width > 0 && (lua_Unsigned)width <= UINT16_MAX / pixel_size, 2, "image width out of range");
    luaL_argcheck(L, height > 0 && height <= UINT16_MAX, 3, "image height out of range");
    size_t row_bytes = (size_t)width * pixel_size;
    luaL_argcheck(L, (size_t)height <= SIZE_MAX / row_bytes && length == row_bytes * (size_t)height, 4, "image data length mismatch");

    lv_obj_t *obj = lua_lvgl_lock_widget(L, ud, LUA_LVGL_OBJ_IMAGE);
    uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)width, format);
    if (stride < row_bytes || stride > UINT16_MAX || (size_t)height > SIZE_MAX / stride || (size_t)height > UINT32_MAX / stride) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl image buffer size out of range");
    }
    size_t size = (size_t)stride * (size_t)height;
    lv_image_dsc_t *dsc = calloc(1, sizeof(*dsc));
    uint8_t *data = calloc(1, size);
    if (!dsc || !data) {
        free(dsc);
        free(data);
        lua_lvgl_unlock();
        ESP_LOGE("lua_lvgl", "Image allocation failed (%zu bytes)", size);
        return luaL_error(L, "lvgl image allocation failed");
    }
    for (size_t y = 0; y < (size_t)height; y++) {
        const uint8_t *in = pixels + y * row_bytes;
        uint8_t *out = data + y * stride;
        if (swapped) {
            for (size_t x = 0; x < row_bytes; x += 2) {
                out[x] = in[x + 1];
                out[x + 1] = in[x];
            }
        } else {
            memcpy(out, in, row_bytes);
        }
    }
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = format;
    dsc->header.w = (uint32_t)width;
    dsc->header.h = (uint32_t)height;
    dsc->header.stride = stride;
    dsc->data_size = (uint32_t)size;
    dsc->data = data;

    lv_image_header_t header;
    if (lv_image_decoder_get_info(dsc, &header) != LV_RESULT_OK) {
        lv_image_cache_drop(dsc);
        free(data);
        free(dsc);
        lua_lvgl_unlock();
        ESP_LOGE("lua_lvgl", "Raw image format rejected (%d)", (int)format);
        return luaL_error(L, "lvgl raw image format not supported");
    }
    /* LVGL borrows both pointers; the object record owns them until replacement or deletion. */
    lua_lvgl_obj_record_t *record = ud->record;
    lv_image_set_src(obj, dsc);
    if (record->data2) lv_image_cache_drop(record->data2);
    free(record->data);
    free(record->data2);
    record->data = data;
    record->data2 = dsc;
    record->data_size = size;
    record->data2_size = sizeof(*dsc);
    record->data_width = (int)width;
    record->data_height = (int)height;
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_image_set_raw_rgb565(lua_State *L)
{
    return image_set_raw(L, LV_COLOR_FORMAT_RGB565);
}

int lua_lvgl_image_set_raw_argb8888(lua_State *L)
{
    return image_set_raw(L, LV_COLOR_FORMAT_ARGB8888);
}

int lua_lvgl_image_set_rotation(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lua_Integer angle = luaL_checkinteger(L, 2) % 3600;
    lv_obj_t *obj = lua_lvgl_lock_widget(L, ud, LUA_LVGL_OBJ_IMAGE);
    lv_image_set_rotation(obj, (int32_t)(angle < 0 ? angle + 3600 : angle));
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_image_set_pivot(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lua_Integer x = luaL_checkinteger(L, 2);
    lua_Integer y = luaL_checkinteger(L, 3);
    luaL_argcheck(L, x >= INT16_MIN && x <= INT16_MAX, 2, "pivot x out of range");
    luaL_argcheck(L, y >= INT16_MIN && y <= INT16_MAX, 3, "pivot y out of range");
    lv_obj_t *obj = lua_lvgl_lock_widget(L, ud, LUA_LVGL_OBJ_IMAGE);
    lv_image_set_pivot(obj, (int32_t)x, (int32_t)y);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}
