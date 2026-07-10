/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_painter_font.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// 颜色格式定义
typedef enum {
    ESP_PAINTER_COLOR_FORMAT_RGB565,
    ESP_PAINTER_COLOR_FORMAT_RGB888,
    ESP_PAINTER_COLOR_FORMAT_YUV420,
    ESP_PAINTER_COLOR_FORMAT_YUV422,
} esp_painter_color_format_t;

// 通用颜色定义
typedef enum {
    ESP_PAINTER_COLOR_BLACK,
    ESP_PAINTER_COLOR_NAVY,
    ESP_PAINTER_COLOR_DARKGREEN,
    ESP_PAINTER_COLOR_DARKCYAN,
    ESP_PAINTER_COLOR_MAROON,
    ESP_PAINTER_COLOR_PURPLE,
    ESP_PAINTER_COLOR_OLIVE,
    ESP_PAINTER_COLOR_LIGHTGREY,
    ESP_PAINTER_COLOR_DARKGREY,
    ESP_PAINTER_COLOR_BLUE,
    ESP_PAINTER_COLOR_GREEN,
    ESP_PAINTER_COLOR_CYAN,
    ESP_PAINTER_COLOR_RED,
    ESP_PAINTER_COLOR_MAGENTA,
    ESP_PAINTER_COLOR_YELLOW,
    ESP_PAINTER_COLOR_WHITE,
    ESP_PAINTER_COLOR_ORANGE,
    ESP_PAINTER_COLOR_GREENYELLOW,
    ESP_PAINTER_COLOR_PINK,
} esp_painter_color_t;

// Color creation macros
#define RGB565(r,g,b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define RGB888(r,g,b) (((r) << 16) | ((g) << 8) | (b))

typedef void* esp_painter_handle_t;

typedef struct {
    struct {
        uint16_t width;
        uint16_t height;
    } canvas;
    esp_painter_color_format_t color_format;
    const esp_painter_basic_font_t *default_font;
    bool swap_rgb565;         // Whether to swap RGB565 bytes (endianness conversion)
} esp_painter_config_t;

esp_err_t esp_painter_init(esp_painter_config_t *config, esp_painter_handle_t *handle);

esp_err_t esp_painter_draw_string(esp_painter_handle_t handle, uint8_t *buffer, uint32_t buffer_size,
                                 uint16_t x, uint16_t y, const esp_painter_basic_font_t *font,
                                 esp_painter_color_t color, const char* text);

esp_err_t esp_painter_draw_string_rgb565(esp_painter_handle_t handle, uint8_t *buffer, uint32_t buffer_size,
                                         uint16_t x, uint16_t y, const esp_painter_basic_font_t *font,
                                         uint16_t color565, const char *text);

esp_err_t esp_painter_draw_string_format(esp_painter_handle_t handle, uint8_t *buffer, uint32_t buffer_size,
                                       uint16_t x, uint16_t y, const esp_painter_basic_font_t *font,
                                       esp_painter_color_t color, const char* fmt, ...);

esp_err_t esp_painter_deinit(esp_painter_handle_t handle);

#ifdef __cplusplus
}
#endif
