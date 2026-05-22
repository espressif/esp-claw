/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lua_image_convert.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LUA_IMAGE_RESIZE_FILTER_NEAREST = 0,
    LUA_IMAGE_RESIZE_FILTER_BILINEAR = 1,
} lua_image_resize_filter_t;

/**
 * @brief Resize an already-decoded image view to @p dst_width x @p dst_height.
 *
 * Only RGB565LE and GRAY8 source formats are supported, and the output format
 * matches the source format. Callers that hold a JPEG / YUYV / RGB888 frame
 * must first lift it to RGB565LE or GRAY8 via lua_image_require_format() /
 * lua_image_convert_view() and then pass the resulting view here.
 *
 * On ESP_OK, @p out owns its own buffer (out->owned == true) and must be
 * released with lua_image_release_view().
 */
esp_err_t lua_image_resize_view(const lua_image_view_t *src,
                                int dst_width,
                                int dst_height,
                                lua_image_resize_filter_t filter,
                                lua_image_view_t *out);

#ifdef __cplusplus
}
#endif
