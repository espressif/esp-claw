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

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} display_color_t;

esp_err_t display_color_from_lua(lua_State *L, int index, display_color_t *out_color);
uint16_t display_color_to_rgb565(display_color_t color);
uint16_t display_color_blend_rgb565(uint16_t dst, display_color_t src);
bool display_color_is_transparent(display_color_t color);
bool display_color_is_opaque(display_color_t color);

#ifdef __cplusplus
}
#endif
