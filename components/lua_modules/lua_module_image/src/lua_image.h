/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lua_image_convert.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Public metadata carried by an image.frame userdata.
 *
 * pixel_format is the FOURCC string ("RGBP", "YUYV", "JPEG", ...). The buffer
 * has room for the longest accepted token plus NUL.
 */
typedef struct {
    int width;
    int height;
    size_t bytes;
    int64_t timestamp_us;
    char pixel_format[8];
} lua_image_frame_info_t;

/**
 * @brief Borrowed view into an existing image.frame userdata.
 *
 * Valid only for the duration of the C call that borrowed it. Do not retain
 * @c data after returning to Lua.
 */
typedef struct {
    const uint8_t *data;
    size_t bytes;
    lua_image_frame_info_t info;
} lua_image_frame_borrow_t;

/**
 * @brief Release hook supplied by a frame producer.
 *
 * Invoked exactly once when the frame transitions to the released state via
 * frame:release(), the to-be-closed (<close>) attribute, or __gc. The
 * producer owns @p ctx and is responsible for any state it carries.
 *
 * @param ctx   Opaque pointer registered at lua_image_push_frame() time.
 * @param data  The buffer pointer that was originally pushed.
 */
typedef void (*lua_image_frame_release_fn_t)(void *ctx, const uint8_t *data);

/**
 * @brief Push a new image.frame userdata onto the Lua stack.
 *
 * On success exactly one userdata is left on top of the stack and ownership
 * of the buffer is transferred to the shared image store (release_cb will be
 * invoked exactly once when the last frame view referencing the store is released).
 *
 * On failure nothing is pushed and release_cb is NOT invoked; the caller
 * still owns @p data and must dispose of it.
 */
esp_err_t lua_image_push_frame(lua_State *L,
                               const uint8_t *data,
                               size_t bytes,
                               const lua_image_frame_info_t *info,
                               lua_image_frame_release_fn_t release_cb,
                               void *release_ctx);

/**
 * @brief Borrow data + info from an image.frame at @p index.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG when the value is not a
 *         frame userdata, ESP_ERR_INVALID_STATE when the frame is released.
 */
esp_err_t lua_image_borrow_frame(lua_State *L, int index, lua_image_frame_borrow_t *out);

/**
 * @brief Read a Lua image.frame and return a view in one requested output format.
 *
 * Lifetime:
 * - Always call lua_image_release_view() when ESP_OK is returned.
 * - Borrowed views are valid only during the current C call and must not be retained.
 */
esp_err_t lua_image_require_format(lua_State *L, int frame_index, lua_image_format_t format, lua_image_view_t *out);

/**
 * @brief Register the image Lua module under the name "image".
 *
 * Exposes:
 *   image.convert(frame, format) -> image.frame view
 *   image.load_file(path) -> image.frame
 *   image.save_file(path, frame)
 */
int luaopen_image(lua_State *L);
esp_err_t lua_module_image_register(void);

#ifdef __cplusplus
}
#endif
