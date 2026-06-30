/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LUA_IMAGE_FORMAT_RGB565LE,
    LUA_IMAGE_FORMAT_RGB565BE,
    LUA_IMAGE_FORMAT_RGB888,
    LUA_IMAGE_FORMAT_BGR888,
    LUA_IMAGE_FORMAT_GRAY8,
    LUA_IMAGE_FORMAT_YUYV,
    LUA_IMAGE_FORMAT_UYVY,
    LUA_IMAGE_FORMAT_JPEG,
    LUA_IMAGE_FORMAT_MJPEG,
    LUA_IMAGE_FORMAT_COUNT,
} lua_image_format_t;

/**
 * @brief Pure C source image description used by the conversion layer.
 */
typedef struct {
    const uint8_t *data;
    size_t bytes;
    int width;
    int height;
    lua_image_format_t format;
    char source_format[8];
} lua_image_source_t;

/**
 * @brief Converted or borrowed image view returned by lua_image_convert_view().
 *
 * When @c owned is true the buffer was allocated by lua_image and must
 * be released via lua_image_release_view(). When false the view
 * borrows the underlying source buffer.
 */
typedef struct {
    const uint8_t *data;              ///< Borrowed or owned image data pointer
    size_t bytes;                     ///< Data size in bytes
    int width;                        ///< Image width in pixels
    int height;                       ///< Image height in pixels
    lua_image_format_t format;        ///< Requested output format
    bool owned;                       ///< True when data must be released through lua_image_release_view()
    char source_format[8];            ///< Original frame pixel_format string
} lua_image_view_t;

/**
 * @brief Convert or borrow one requested output format from a pure C source image.
 *
 * Lifetime:
 * - Always call lua_image_release_view() when ESP_OK is returned.
 * - Borrowed views are valid only while the source image buffer remains valid.
 */
esp_err_t lua_image_convert_view(const lua_image_source_t *src, lua_image_format_t format, lua_image_view_t *out);

/**
 * @brief Release an lua_image_view_t returned by conversion helpers.
 */
void lua_image_release_view(lua_image_view_t *view);

const char *lua_image_format_name(lua_image_format_t format);

#ifdef __cplusplus
}
#endif
