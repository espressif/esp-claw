/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_painter.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#undef CONFIG_SOC_PPA_SUPPORTED
#define CONFIG_SOC_PPA_SUPPORTED 0
#if CONFIG_SOC_PPA_SUPPORTED
#include "esp_private/esp_cache_private.h"
#include "driver/ppa.h"
#endif

static const char *TAG = "esp_painter";

#if CONFIG_SOC_PPA_SUPPORTED
#define ALIGN_UP(size) (((size) + data_cache_line_size - 1) & ~(data_cache_line_size - 1))
#endif

typedef struct {
    struct {
        uint16_t width;
        uint16_t height;
    } canvas;
    struct {
        uint16_t width;
    uint16_t height;
    } text_canvas;
    esp_painter_color_format_t color_format;
    const esp_painter_basic_font_t *default_font;
    bool swap_rgb565;         // Whether to swap RGB565 bytes
#if CONFIG_SOC_PPA_SUPPORTED
    ppa_client_handle_t ppa_client;
    uint8_t *ppa_output_buffer;
#endif
} esp_painter_t;

#if CONFIG_SOC_PPA_SUPPORTED
static size_t data_cache_line_size = 0;
#endif

static const struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_colors[] = {
    {0x00, 0x00, 0x00},  // BLACK
    {0x00, 0x00, 0x80},  // NAVY
    {0x00, 0x80, 0x00},  // DARKGREEN
    {0x00, 0x80, 0x80},  // DARKCYAN
    {0x80, 0x00, 0x00},  // MAROON
    {0x80, 0x00, 0x80},  // PURPLE
    {0x80, 0x80, 0x00},  // OLIVE
    {0xC0, 0xC0, 0xC0},  // LIGHTGREY
    {0x80, 0x80, 0x80},  // DARKGREY
    {0x00, 0x00, 0xFF},  // BLUE
    {0x00, 0xFF, 0x00},  // GREEN
    {0x00, 0xFF, 0xFF},  // CYAN
    {0xFF, 0x00, 0x00},  // RED
    {0xFF, 0x00, 0xFF},  // MAGENTA
    {0xFF, 0xFF, 0x00},  // YELLOW
    {0xFF, 0xFF, 0xFF},  // WHITE
    {0xFF, 0xA5, 0x00},  // ORANGE
    {0xAD, 0xFF, 0x2F},  // GREENYELLOW
    {0xFF, 0xC0, 0xCB},  // PINK
};

static uint32_t esp_painter_get_color(esp_painter_handle_t handle, esp_painter_color_t color)
{
    esp_painter_t *painter = (esp_painter_t *)handle;
    if (!painter || color >= sizeof(rgb_colors) / sizeof(rgb_colors[0])) {
        return 0;
    }

    uint8_t r = rgb_colors[color].r;
    uint8_t g = rgb_colors[color].g;
    uint8_t b = rgb_colors[color].b;

    uint32_t color_value = 0;
    switch (painter->color_format) {
        case ESP_PAINTER_COLOR_FORMAT_YUV420:
        case ESP_PAINTER_COLOR_FORMAT_YUV422:
        case ESP_PAINTER_COLOR_FORMAT_RGB565:
            color_value = RGB565(r, g, b);
            // Swap bytes if needed for RGB565
            if (painter->swap_rgb565) {
                color_value = ((color_value & 0xFF) << 8) | ((color_value >> 8) & 0xFF);
            }
            return color_value;
        case ESP_PAINTER_COLOR_FORMAT_RGB888:
            return RGB888(r, g, b);
        default:
            return 0;
    }
}

static uint16_t esp_painter_get_rgb565_color_value(esp_painter_handle_t handle, uint16_t color565)
{
    esp_painter_t *painter = (esp_painter_t *)handle;
    if (!painter) {
        return 0;
    }

    if (painter->swap_rgb565) {
        return (uint16_t)((color565 << 8) | (color565 >> 8));
    }

    return color565;
}

static void esp_painter_measure_text(const esp_painter_basic_font_t *font, const char *text,
                                     uint16_t *out_width, uint16_t *out_height)
{
    uint16_t max_cols = 0;
    uint16_t cols = 0;
    uint16_t lines = 1;

    if (!font || !text) {
        if (out_width) {
            *out_width = 0;
        }
        if (out_height) {
            *out_height = 0;
        }
        return;
    }

    for (const char *p = text; *p; ++p) {
        if (*p == '\n') {
            if (cols > max_cols) {
                max_cols = cols;
            }
            cols = 0;
            lines++;
        } else if (*p != '\r') {
            cols++;
        }
    }
    if (cols > max_cols) {
        max_cols = cols;
    }

    if (out_width) {
        *out_width = (uint16_t)(max_cols * font->width);
    }
    if (out_height) {
        *out_height = (uint16_t)(lines * font->height);
    }
}

#if CONFIG_SOC_PPA_SUPPORTED
static esp_err_t esp_painter_convert_yuv420_to_rgb565(esp_painter_handle_t handle, uint8_t *in_buffer, uint8_t *out_buffer,
                                            uint32_t out_buffer_size,
                                            uint32_t block_w, uint32_t block_h,
                                            uint32_t block_offset_x, uint32_t block_offset_y)
{
    esp_painter_t *painter = (esp_painter_t *)handle;
    if (!painter || !in_buffer || !out_buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    ppa_srm_oper_config_t srm_config = {
        .in.buffer = in_buffer,
        .in.pic_w = painter->canvas.width,
        .in.pic_h = painter->canvas.height,
        .in.block_w = block_w,
        .in.block_h = block_h,
        .in.block_offset_x = block_offset_x,
        .in.block_offset_y = block_offset_y,
        .in.srm_cm = PPA_SRM_COLOR_MODE_YUV420,
        .out.buffer = out_buffer,
        .out.buffer_size = out_buffer_size,
        .out.pic_w = block_w,
        .out.pic_h = block_h,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1,
        .scale_y = 1,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    int ret = ppa_do_scale_rotate_mirror(painter->ppa_client, &srm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "First PPA conversion failed");
    }

    return ret;
}

static esp_err_t esp_painter_convert_rgb565_to_yuv420(esp_painter_handle_t handle, uint8_t *in_buffer, uint8_t *out_buffer, uint32_t buffer_size,
                                            uint32_t in_width, uint32_t in_height,
                                            uint32_t out_width, uint32_t out_height,
                                            uint32_t out_offset_x, uint32_t out_offset_y)
{
    esp_painter_t *painter = (esp_painter_t *)handle;
    if (!painter || !in_buffer || !out_buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    ppa_srm_oper_config_t srm_out_config = {
        .in.buffer = in_buffer,
        .in.pic_w = in_width,
        .in.pic_h = in_height,
        .in.block_w = in_width,
        .in.block_h = in_height,
        .in.block_offset_x = 0,
        .in.block_offset_y = 0,
        .in.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        .out.buffer = out_buffer,
        .out.buffer_size = buffer_size,
        .out.pic_w = out_width,
        .out.pic_h = out_height,
        .out.block_offset_x = out_offset_x,
        .out.block_offset_y = out_offset_y,
        .out.srm_cm = PPA_SRM_COLOR_MODE_YUV420,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1,
        .scale_y = 1,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    int ret = ppa_do_scale_rotate_mirror(painter->ppa_client, &srm_out_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Second PPA conversion failed");
    }

    return ESP_OK;
}
#endif

static esp_err_t esp_painter_draw_pixel(esp_painter_handle_t handle, uint8_t *buffer, uint32_t buffer_size,
                           uint16_t x, uint16_t y, uint16_t color_value)
{
    esp_painter_t *painter = (esp_painter_t *)handle;
    if (!painter || !buffer) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_SOC_PPA_SUPPORTED
    bool using_ppa = (painter->color_format == ESP_PAINTER_COLOR_FORMAT_YUV420 ||
                     painter->color_format == ESP_PAINTER_COLOR_FORMAT_YUV422);
#else
    bool using_ppa = false;
#endif

    if (using_ppa) {
        ESP_LOGD(TAG, "Drawing pixel at (%d,%d) with color 0x%04" PRIx32, x, y, color_value);

        uint32_t offset = (y * painter->text_canvas.width + x) * 2;
        if (offset + 2 > buffer_size) {
            ESP_LOGW(TAG, "Buffer overflow: offset %" PRIu32 ", size %" PRIu32, offset, buffer_size);
            return ESP_ERR_INVALID_SIZE;
        }

        uint8_t low_byte = color_value & 0xFF;
        uint8_t high_byte = (color_value >> 8) & 0xFF;
        buffer[offset] = low_byte;
        buffer[offset + 1] = high_byte;

        return ESP_OK;
    }

    switch (painter->color_format) {
        case ESP_PAINTER_COLOR_FORMAT_RGB565: {
            uint32_t offset = (y * painter->canvas.width + x) * 2;
            if (offset + 2 > buffer_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            buffer[offset] = color_value & 0xFF;
            buffer[offset + 1] = (color_value >> 8) & 0xFF;
            break;
        }
        case ESP_PAINTER_COLOR_FORMAT_RGB888: {
            uint32_t offset = (y * painter->canvas.width + x) * 3;
            if (offset + 3 > buffer_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            buffer[offset] = color_value & 0xFF;
            buffer[offset + 1] = (color_value >> 8) & 0xFF;
            buffer[offset + 2] = (color_value >> 16) & 0xFF;
            break;
        }
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

static esp_err_t esp_painter_draw_char(esp_painter_handle_t handle, uint8_t *buffer, uint32_t buffer_size,
                               uint16_t x, uint16_t y, const esp_painter_basic_font_t *font,
                               uint16_t color_value, char c)
{
    esp_painter_t *painter = (esp_painter_t *)handle;
    if (!painter || !buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!font) {
        font = painter->default_font;
    }
    if (!font) {
        ESP_LOGE(TAG, "No font specified");
        return ESP_ERR_INVALID_ARG;
    }

    if (c < 32 || c > 127) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t char_offset = (c - 32) * font->height * ((font->width + 7) / 8);
    const uint8_t *char_bitmap = font->bitmap + char_offset;
    ESP_LOGD(TAG, "Drawing char '%c' at (%d,%d)", c, x, y);

    for (int dy = 0; dy < font->height; dy++) {
        for (int dx = 0; dx < font->width; dx++) {
            uint8_t byte = char_bitmap[dy * ((font->width + 7) / 8) + (dx / 8)];
            if (byte & (0x80 >> (dx % 8))) {
                ESP_LOGD(TAG, "Setting pixel at relative pos (%d,%d)", dx, dy);
                esp_err_t ret = esp_painter_draw_pixel(handle, buffer, buffer_size, x + dx, y + dy, color_value);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to draw pixel at (%d,%d)", x + dx, y + dy);
                    return ret;
                }
            }
        }
    }

    return ESP_OK;
}

static esp_err_t esp_painter_draw_text(esp_painter_t *painter, uint8_t *buffer,
                                uint32_t buffer_size, uint16_t x, uint16_t y,
                                uint32_t text_width, uint32_t text_height,
                                const esp_painter_basic_font_t *font,
                                uint16_t color_value, const char *text)
{
    uint16_t cursor_x = x;
    uint16_t cursor_y = y;
    esp_err_t ret = ESP_OK;

    while (*text) {
        if (cursor_y >= text_height) {
            break;
        }

        switch (*text) {
            case '\n':
                cursor_x = x;
                cursor_y += font->height;
                break;
            case '\r':
                cursor_x = x;
                break;
            default:
                if (cursor_x + font->width <= text_width) {
                    ret = esp_painter_draw_char(painter, buffer, buffer_size,
                                              cursor_x, cursor_y, font, color_value, *text);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to draw char at (%d,%d)", cursor_x, cursor_y);
                        return ret;
                    }
                }
                cursor_x += font->width;

                if (cursor_x >= text_width) {
                    cursor_x = x;
                    cursor_y += font->height;
                }
                break;
        }
        text++;
    }
    return ret;
}

esp_err_t esp_painter_draw_string(esp_painter_handle_t handle, uint8_t *buffer, uint32_t buffer_size,
                                 uint16_t x, uint16_t y, const esp_painter_basic_font_t *font,
                                 esp_painter_color_t color, const char* text)
{
    esp_err_t ret = ESP_OK;
    esp_painter_t *painter = (esp_painter_t *)handle;
    if (!painter || !buffer || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!font) {
        font = painter->default_font;
        if (!font) {
            ESP_LOGE(TAG, "No font specified");
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (x >= painter->canvas.width || y >= painter->canvas.height) {
        ESP_LOGW(TAG, "Starting position out of bounds: (%d,%d)", x, y);
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t color_value = (uint16_t)esp_painter_get_color(handle, color);

#if CONFIG_SOC_PPA_SUPPORTED
    bool using_ppa = (painter->color_format == ESP_PAINTER_COLOR_FORMAT_YUV420 ||
                     painter->color_format == ESP_PAINTER_COLOR_FORMAT_YUV422);
    if (using_ppa) {
        uint16_t text_width = strlen(text) * font->width;
        uint16_t text_height = font->height;

        painter->text_canvas.width = text_width;
        painter->text_canvas.height = text_height;

        uint32_t aligned_size = ALIGN_UP(text_width * text_height * 2);
        if (aligned_size > buffer_size) {
            ESP_LOGE(TAG, "Buffer too small: need %" PRIu32 ", have %" PRIu32, aligned_size, buffer_size);
            return ESP_ERR_INVALID_SIZE;
        }

        uint8_t *ppa_output_buffer = heap_caps_aligned_alloc(
            data_cache_line_size,
            aligned_size,
            MALLOC_CAP_SPIRAM
        );
        if (!ppa_output_buffer) {
            ESP_LOGE(TAG, "Failed to allocate PPA output buffer");
            return ESP_ERR_NO_MEM;
        }

        int ret = esp_painter_convert_yuv420_to_rgb565(painter, buffer, ppa_output_buffer,
                                                    aligned_size,
                                                    text_width, text_height,
                                                    x, y);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "First PPA conversion failed");
            goto cleanup;
        }

        ret = esp_painter_draw_text(painter, ppa_output_buffer, aligned_size, 0, 0, text_width, text_height, font, color_value, text);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to draw text");
            goto cleanup;
        }

        ret = esp_painter_convert_rgb565_to_yuv420(painter, ppa_output_buffer, buffer,
                                                    buffer_size,
                                                    text_width, text_height,
                                                    painter->canvas.width, painter->canvas.height,
                                                    x, y);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Second PPA conversion failed");
            goto cleanup;
        }

cleanup:
        if (ppa_output_buffer) {
            heap_caps_free(ppa_output_buffer);
            ppa_output_buffer = NULL;
        }

        if (ret != ESP_OK) {
            return ret;
        }
    } else {
        ret = esp_painter_draw_text(painter, buffer, buffer_size, x, y, painter->canvas.width, painter->canvas.height, font, color_value, text);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to draw text");
            return ret;
        }
    }
#else
    ret = esp_painter_draw_text(painter, buffer, buffer_size, x, y, painter->canvas.width, painter->canvas.height, font, color_value, text);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to draw text");
        return ret;
    }
#endif

    return ret;
}

esp_err_t esp_painter_draw_string_rgb565(esp_painter_handle_t handle, uint8_t *buffer, uint32_t buffer_size,
                                         uint16_t x, uint16_t y, const esp_painter_basic_font_t *font,
                                         uint16_t color565, const char *text)
{
    esp_painter_t *painter = (esp_painter_t *)handle;
    esp_painter_t local_painter;
    uint16_t text_width = 0;
    uint16_t text_height = 0;
    esp_err_t ret;

    if (!painter || !buffer || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    if (painter->color_format != ESP_PAINTER_COLOR_FORMAT_RGB565) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!font) {
        font = painter->default_font;
        if (!font) {
            ESP_LOGE(TAG, "No font specified");
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (x >= painter->canvas.width || y >= painter->canvas.height) {
        ESP_LOGW(TAG, "Starting position out of bounds: (%d,%d)", x, y);
        return ESP_ERR_INVALID_ARG;
    }

    esp_painter_measure_text(font, text, &text_width, &text_height);
    if (text_width == 0 || text_height == 0) {
        return ESP_OK;
    }
    if (buffer_size < (uint32_t)text_width * text_height * 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    local_painter = *painter;
    local_painter.canvas.width = text_width;
    local_painter.canvas.height = text_height;

    ret = esp_painter_draw_text(&local_painter, buffer, buffer_size, 0, 0,
                                text_width, text_height,
                                font, esp_painter_get_rgb565_color_value(handle, color565), text);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to draw RGB565 text");
        return ret;
    }

    return ret;
}

esp_err_t esp_painter_draw_string_format(esp_painter_handle_t handle, uint8_t *buffer, uint32_t buffer_size,
                                       uint16_t x, uint16_t y, const esp_painter_basic_font_t *font,
                                       esp_painter_color_t color, const char* fmt, ...)
{
    char buf[CONFIG_ESP_PAINTER_FORMAT_SIZE_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    return esp_painter_draw_string(handle, buffer, buffer_size, x, y, font, color, buf);
}

esp_err_t esp_painter_init(esp_painter_config_t *config, esp_painter_handle_t *handle)
{
    if (!config || !handle) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    esp_painter_t *painter = (esp_painter_t *)calloc(1, sizeof(esp_painter_t));
    if (!painter) {
        ESP_LOGE(TAG, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }

    painter->canvas.width = config->canvas.width;
    painter->canvas.height = config->canvas.height;
    painter->color_format = config->color_format;
    painter->default_font = config->default_font;
    painter->swap_rgb565 = config->swap_rgb565;

    if (config->color_format == ESP_PAINTER_COLOR_FORMAT_YUV420 ||
        config->color_format == ESP_PAINTER_COLOR_FORMAT_YUV422) {
#if CONFIG_SOC_PPA_SUPPORTED
        ppa_client_config_t ppa_srm_config = {
            .oper_type = PPA_OPERATION_SRM,
        };
        ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &painter->ppa_client));
        ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size));
#else
        free(painter);
        ESP_LOGE(TAG, "YUV painter mode requires PPA support on this target");
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }

    *handle = painter;
    ESP_LOGI(TAG, "Painter initialized: %dx%d, format: %d",
             config->canvas.width, config->canvas.height, config->color_format);
    return ESP_OK;
}

esp_err_t esp_painter_deinit(esp_painter_handle_t handle)
{
    esp_painter_t *painter = (esp_painter_t *)handle;
    if (!painter) {
        return ESP_ERR_INVALID_ARG;
    }


#if CONFIG_SOC_PPA_SUPPORTED
    if(painter->ppa_client) {
        ppa_unregister_client(painter->ppa_client);
        painter->ppa_client = NULL;
    }
#endif

    free(painter);
    return ESP_OK;
}
