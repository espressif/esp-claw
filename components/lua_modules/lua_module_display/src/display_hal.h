/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* Display Hardware Abstraction Layer
 *
 * Declares the interface that the board/application layer must implement.
 * The lua_module_display component calls these functions to perform display
 * operations without depending on any specific LCD driver.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "display_color.h"
#include "display_service.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_HAL_TEXT_ALIGN_LEFT = 0,
    DISPLAY_HAL_TEXT_ALIGN_CENTER,
    DISPLAY_HAL_TEXT_ALIGN_RIGHT,
} display_hal_text_align_t;

typedef enum {
    DISPLAY_HAL_TEXT_VALIGN_TOP = 0,
    DISPLAY_HAL_TEXT_VALIGN_MIDDLE,
    DISPLAY_HAL_TEXT_VALIGN_BOTTOM,
} display_hal_text_valign_t;

typedef struct {
    uint8_t framebuffer_count;
    bool double_buffered;
    bool frame_active;
    bool flush_in_flight;
} display_hal_animation_info_t;

typedef enum {
    DISPLAY_HAL_PANEL_IF_IO = 0,
    DISPLAY_HAL_PANEL_IF_RGB,
    DISPLAY_HAL_PANEL_IF_MIPI_DSI,
} display_hal_panel_if_t;

/**
 * @brief Framebuffer pixel format.
 *
 * Only RGB565 (2 bytes/pixel input as RGB565 little-endian) and RGB888
 * (3 bytes/pixel input as R,G,B) are supported. RGB565 remains standard RGB
 * order; RGB888 is stored/submitted in native BGR panel order.
 */
typedef enum {
    DISPLAY_HAL_PIXEL_FORMAT_RGB565 = 0,
    DISPLAY_HAL_PIXEL_FORMAT_RGB888,
} display_hal_pixel_format_t;

/**
 * @brief Number of bytes per pixel for a given format.
 */
size_t display_hal_pixel_format_bytes(display_hal_pixel_format_t format);

/**
 * @brief One-time module init. Pre-creates the HAL mutex. Idempotent; must be
 *        called during module registration before any other HAL API.
 */
esp_err_t display_hal_module_init(void);

esp_err_t display_hal_create(display_service_session_handle_t session,
                             esp_lcd_panel_handle_t panel_handle,
                             esp_lcd_panel_io_handle_t io_handle,
                             display_hal_panel_if_t panel_if,
                             display_hal_pixel_format_t pixel_format,
                             int lcd_width,
                             int lcd_height);
esp_err_t display_hal_destroy(void);

/**
 * @brief Current framebuffer pixel format.
 *
 * Returns @c DISPLAY_HAL_PIXEL_FORMAT_RGB565 when the HAL is not yet created.
 */
display_hal_pixel_format_t display_hal_get_pixel_format(void);

int display_hal_width(void);
int display_hal_height(void);

esp_err_t display_hal_begin_frame(bool clear, display_color_t color, bool preserve);
esp_err_t display_hal_present(void);
esp_err_t display_hal_present_full(void);
esp_err_t display_hal_end_frame(void);
bool display_hal_is_frame_active(void);
esp_err_t display_hal_get_animation_info(display_hal_animation_info_t *info);

esp_err_t display_hal_clear(display_color_t color);
esp_err_t display_hal_set_clip_rect(int x, int y, int width, int height);
esp_err_t display_hal_clear_clip_rect(void);
esp_err_t display_hal_fill_rect(int x, int y, int width, int height, display_color_t color);
esp_err_t display_hal_draw_line(int x0, int y0, int x1, int y1, display_color_t color);
esp_err_t display_hal_draw_rect(int x, int y, int width, int height, display_color_t color);
esp_err_t display_hal_draw_pixel(int x, int y, display_color_t color);
esp_err_t display_hal_set_backlight(bool on);
esp_err_t display_hal_fill_circle(int cx, int cy, int r, display_color_t color);
esp_err_t display_hal_draw_circle(int cx, int cy, int r, display_color_t color);
esp_err_t display_hal_draw_arc(int cx, int cy, int radius,
                               float start_deg, float end_deg, display_color_t color);
esp_err_t display_hal_fill_arc(int cx, int cy, int inner_radius, int outer_radius,
                               float start_deg, float end_deg, display_color_t color);
esp_err_t display_hal_draw_ellipse(int cx, int cy, int radius_x, int radius_y,
                                   display_color_t color);
esp_err_t display_hal_fill_ellipse(int cx, int cy, int radius_x, int radius_y,
                                   display_color_t color);
esp_err_t display_hal_draw_round_rect(int x, int y, int width, int height,
                                      int radius, display_color_t color);
esp_err_t display_hal_fill_round_rect(int x, int y, int width, int height,
                                      int radius, display_color_t color);
esp_err_t display_hal_draw_triangle(int x1, int y1, int x2, int y2,
                                    int x3, int y3, display_color_t color);
esp_err_t display_hal_fill_triangle(int x1, int y1, int x2, int y2,
                                    int x3, int y3, display_color_t color);

esp_err_t display_hal_measure_text(const char *text, uint8_t font_size,
                                   uint16_t *out_width, uint16_t *out_height);
esp_err_t display_hal_draw_text(int x, int y, const char *text, uint8_t font_size,
                                display_color_t text_color, bool has_bg, display_color_t bg_color);
esp_err_t display_hal_draw_text_aligned(int x, int y, int width, int height,
                                        const char *text, uint8_t font_size,
                                        display_color_t text_color, bool has_bg, display_color_t bg_color,
                                        display_hal_text_align_t align,
                                        display_hal_text_valign_t valign);


esp_err_t display_hal_draw_bitmap(int x, int y, int w, int h,
                                  const void *pixels,
                                  display_hal_pixel_format_t src_format);
esp_err_t display_hal_draw_bitmap_native(int x, int y, int w, int h,
                                         const void *pixels,
                                         display_hal_pixel_format_t src_format);
esp_err_t display_hal_draw_bitmap_crop(int x, int y,
                                       int src_x, int src_y,
                                       int w, int h,
                                       int src_width, int src_height,
                                       const void *pixels,
                                       display_hal_pixel_format_t src_format);
esp_err_t display_hal_draw_bitmap_crop_native(int x, int y,
                                              int src_x, int src_y,
                                              int w, int h,
                                              int src_width, int src_height,
                                              const void *pixels,
                                              display_hal_pixel_format_t src_format);
esp_err_t display_hal_draw_bitmap_scaled(int x, int y,
                                         const void *pixels,
                                         int src_width, int src_height,
                                         int scale_w, int scale_h,
                                         display_hal_pixel_format_t src_format,
                                         int *out_w, int *out_h);
esp_err_t display_hal_draw_bitmap_scaled_native(int x, int y,
                                                const void *pixels,
                                                int src_width, int src_height,
                                                int scale_w, int scale_h,
                                                display_hal_pixel_format_t src_format,
                                                int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif
