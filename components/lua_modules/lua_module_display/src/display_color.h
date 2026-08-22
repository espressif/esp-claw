/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Native display color.
 *
 * The memory layout follows the panel-native BGR order used by LVGL:
 * byte 0 = blue, byte 1 = green, byte 2 = red, byte 3 = alpha.
 */
typedef union {
    struct {
        uint8_t b;
        uint8_t g;
        uint8_t r;
        uint8_t a;
    };
    struct {
        uint8_t blue;
        uint8_t green;
        uint8_t red;
        uint8_t alpha;
    };
    uint8_t channel[4];
    uint32_t full;
} display_color_t;

esp_err_t display_color_from_lua(lua_State *L, int index, display_color_t *out_color);
uint16_t display_color_to_rgb565(display_color_t color);
uint16_t display_color_blend_rgb565(uint16_t dst, display_color_t src);

/**
 * @brief Pack a color into a native 24-bit BGR888 value.
 *
 * The returned value is packed as 0x00BBGGRR. Alpha is ignored.
 */
uint32_t display_color_to_rgb888(display_color_t color);

/**
 * @brief Alpha-blend @p src over an existing RGB888 pixel.
 *
 * @param dst  Existing pixel value packed as 0x00BBGGRR.
 * @param src  Source color, uses @c src.a as alpha weight.
 * @return     Blended pixel packed as 0x00BBGGRR.
 */
uint32_t display_color_blend_rgb888(uint32_t dst, display_color_t src);

bool display_color_is_transparent(display_color_t color);
bool display_color_is_opaque(display_color_t color);

#ifdef __cplusplus
}
#endif
