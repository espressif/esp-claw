/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_image_convert.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_check.h"

#define LUA_IMAGE_JPEG_QUALITY 80
#define LUA_IMAGE_MIN_JPEG_BUFFER_SIZE (64 * 1024)
#define LUA_IMAGE_MAX_PIXELS (1920U * 1080U)

static const char *TAG = "image_convert";

const char *lua_image_format_name(lua_image_format_t format)
{
    switch (format) {
    case LUA_IMAGE_FORMAT_RGB565LE:
        return "RGB565LE";
    case LUA_IMAGE_FORMAT_RGB565BE:
        return "RGB565BE";
    case LUA_IMAGE_FORMAT_RGB888:
        return "RGB888";
    case LUA_IMAGE_FORMAT_BGR888:
        return "BGR888";
    case LUA_IMAGE_FORMAT_GRAY8:
        return "GRAY8";
    case LUA_IMAGE_FORMAT_YUYV:
        return "YUYV";
    case LUA_IMAGE_FORMAT_UYVY:
        return "UYVY";
    case LUA_IMAGE_FORMAT_JPEG:
        return "JPEG";
    case LUA_IMAGE_FORMAT_MJPEG:
        return "MJPEG";
    default:
        return "UNKNOWN";
    }
}

void lua_image_release_view(lua_image_view_t *view)
{
    if (!view) {
        return;
    }
    if (view->owned) {
        free((void *)view->data);
    }
    memset(view, 0, sizeof(*view));
}

static uint8_t lua_image_clip_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static uint16_t lua_image_rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

static uint8_t lua_image_rgb888_to_gray(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint8_t)(((uint16_t)r * 77 + (uint16_t)g * 150 + (uint16_t)b * 29) >> 8);
}

static uint8_t lua_image_rgb565_to_gray(uint16_t pixel)
{
    uint16_t r = (uint16_t)(((pixel >> 8) & 0xf8) | ((pixel >> 13) & 0x07));
    uint16_t g = (uint16_t)(((pixel >> 3) & 0xfc) | ((pixel >> 9) & 0x03));
    uint16_t b = (uint16_t)(((pixel << 3) & 0xf8) | ((pixel >> 2) & 0x07));

    /* Approximate BT.601 luma with shift-based RGB565 expansion and no per-pixel division. */
    return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
}

static uint16_t lua_image_yuv_to_rgb565(int y, int u, int v)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    uint8_t r;
    uint8_t g;
    uint8_t b;

    if (c < 0) {
        c = 0;
    }
    r = lua_image_clip_u8((298 * c + 409 * e + 128) >> 8);
    g = lua_image_clip_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    b = lua_image_clip_u8((298 * c + 516 * d + 128) >> 8);
    return lua_image_rgb888_to_rgb565(r, g, b);
}

static esp_err_t lua_image_checked_pixel_count(int width, int height, size_t *out_pixels)
{
    size_t pixels;

    if (out_pixels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_pixels = 0;
    if (width <= 0 || height <= 0 || (size_t)width > SIZE_MAX / (size_t)height) {
        ESP_LOGE(TAG, "invalid image dimensions: %dx%d", width, height);
        return ESP_ERR_INVALID_SIZE;
    }

    pixels = (size_t)width * (size_t)height;
    if (pixels == 0 || pixels > LUA_IMAGE_MAX_PIXELS) {
        ESP_LOGE(TAG, "image exceeds pixel limit: %u pixels (limit=%u)", (unsigned)pixels, (unsigned)LUA_IMAGE_MAX_PIXELS);
        return ESP_ERR_INVALID_SIZE;
    }

    *out_pixels = pixels;
    return ESP_OK;
}

static esp_err_t lua_image_checked_data_bytes(size_t pixels, size_t bytes_per_pixel, size_t *out_bytes)
{
    if (out_bytes == NULL || bytes_per_pixel == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pixels > SIZE_MAX / bytes_per_pixel) {
        ESP_LOGE(TAG, "image byte size overflow: pixels=%u bpp=%u", (unsigned)pixels, (unsigned)bytes_per_pixel);
        return ESP_ERR_INVALID_SIZE;
    }
    *out_bytes = pixels * bytes_per_pixel;
    return ESP_OK;
}

static void lua_image_init_owned_view(const lua_image_source_t *src, lua_image_view_t *out, uint8_t *data, size_t bytes, lua_image_format_t format)
{
    out->data = data;
    out->bytes = bytes;
    out->width = src->width;
    out->height = src->height;
    out->format = format;
    out->owned = true;
    strlcpy(out->source_format, src->source_format, sizeof(out->source_format));
}

static void lua_image_init_borrowed_view(const lua_image_source_t *src, lua_image_view_t *out, lua_image_format_t format)
{
    out->data = src->data;
    out->bytes = src->bytes;
    out->width = src->width;
    out->height = src->height;
    out->format = format;
    out->owned = false;
    strlcpy(out->source_format, src->source_format, sizeof(out->source_format));
}

static esp_err_t lua_image_decode_jpeg_to_rgb565le(const lua_image_source_t *src, lua_image_view_t *out)
{
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_dec_handle_t decoder = NULL;
    jpeg_dec_io_t io = {0};
    jpeg_dec_header_info_t header = {0};
    uint8_t *pixels = NULL;
    size_t pixel_count = 0;
    int out_len = 0;
    jpeg_error_t jpeg_err;
    esp_err_t err;

    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_err = jpeg_dec_open(&config, &decoder);
    if (jpeg_err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open failed: %d", jpeg_err);
        return ESP_FAIL;
    }
    if (src->bytes > INT_MAX) {
        ESP_LOGE(TAG, "JPEG input too large: %u bytes", (unsigned)src->bytes);
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }

    io.inbuf = (uint8_t *)src->data;
    io.inbuf_len = (int)src->bytes;
    jpeg_err = jpeg_dec_parse_header(decoder, &io, &header);
    if (jpeg_err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_parse_header failed: %d", jpeg_err);
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = lua_image_checked_pixel_count((int)header.width, (int)header.height, &pixel_count);
    if (err != ESP_OK || header.height == 0 || header.width > (uint32_t)INT_MAX / 2 / header.height) {
        ESP_LOGE(TAG, "JPEG header invalid size: %ux%u", (unsigned)header.width, (unsigned)header.height);
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }

    out_len = (int)header.width * (int)header.height * 2;
    pixels = (uint8_t *)heap_caps_aligned_calloc(16, 1, (size_t)out_len, MALLOC_CAP_8BIT|MALLOC_CAP_SPIRAM);
    if (!pixels) {
        ESP_LOGE(TAG, "JPEG decode RGB565 buffer alloc failed: %u bytes", (unsigned)out_len);
        jpeg_dec_close(decoder);
        return ESP_ERR_NO_MEM;
    }

    io.outbuf = pixels;
    jpeg_err = jpeg_dec_process(decoder, &io);
    jpeg_dec_close(decoder);
    if (jpeg_err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_process failed: %d", jpeg_err);
        free(pixels);
        return ESP_FAIL;
    }

    out->data = pixels;
    out->bytes = (size_t)out_len;
    out->width = (int)header.width;
    out->height = (int)header.height;
    out->format = LUA_IMAGE_FORMAT_RGB565LE;
    out->owned = true;
    strlcpy(out->source_format, src->source_format, sizeof(out->source_format));
    return ESP_OK;
}

static esp_err_t lua_image_require_rgb565le(const lua_image_source_t *src, lua_image_view_t *out)
{
    size_t pixel_count = 0;
    size_t required_bytes = 0;
    uint16_t *pixels = NULL;
    esp_err_t err = lua_image_checked_pixel_count(src->width, src->height, &pixel_count);

    if (err != ESP_OK) {
        return err;
    }

    if (src->format == LUA_IMAGE_FORMAT_JPEG || src->format == LUA_IMAGE_FORMAT_MJPEG) {
        return lua_image_decode_jpeg_to_rgb565le(src, out);
    }
    if (src->format == LUA_IMAGE_FORMAT_RGB565LE) {
        ESP_RETURN_ON_ERROR(lua_image_checked_data_bytes(pixel_count, 2, &required_bytes), TAG, "RGB565 size check failed");
        lua_image_init_borrowed_view(src, out, LUA_IMAGE_FORMAT_RGB565LE);
        return src->bytes >= required_bytes ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    ESP_RETURN_ON_ERROR(lua_image_checked_data_bytes(pixel_count, sizeof(uint16_t), &required_bytes), TAG, "RGB565 alloc size check failed");
    pixels = (uint16_t *)heap_caps_aligned_calloc(16, 1, required_bytes, MALLOC_CAP_8BIT|MALLOC_CAP_SPIRAM);
    if (!pixels) {
        ESP_LOGE(TAG, "display conversion buffer alloc failed: %u bytes", (unsigned)required_bytes);
        return ESP_ERR_NO_MEM;
    }

    switch (src->format) {
    case LUA_IMAGE_FORMAT_RGB565BE:
        if (src->bytes < required_bytes) {
            free(pixels);
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t i = 0; i < pixel_count; i++) {
            pixels[i] = ((uint16_t)src->data[i * 2] << 8) | src->data[i * 2 + 1];
        }
        break;
    case LUA_IMAGE_FORMAT_RGB888:
    case LUA_IMAGE_FORMAT_BGR888:
        err = lua_image_checked_data_bytes(pixel_count, 3, &required_bytes);
        if (err != ESP_OK) {
            free(pixels);
            return err;
        }
        if (src->bytes < required_bytes) {
            free(pixels);
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t i = 0; i < pixel_count; i++) {
            const uint8_t *p = src->data + i * 3;
            pixels[i] = src->format == LUA_IMAGE_FORMAT_RGB888 ?
                lua_image_rgb888_to_rgb565(p[0], p[1], p[2]) :
                lua_image_rgb888_to_rgb565(p[2], p[1], p[0]);
        }
        break;
    case LUA_IMAGE_FORMAT_GRAY8:
        if (src->bytes < pixel_count) {
            free(pixels);
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t i = 0; i < pixel_count; i++) {
            pixels[i] = lua_image_rgb888_to_rgb565(src->data[i], src->data[i], src->data[i]);
        }
        break;
    case LUA_IMAGE_FORMAT_YUYV:
    case LUA_IMAGE_FORMAT_UYVY:
        err = lua_image_checked_data_bytes(pixel_count, 2, &required_bytes);
        if (err != ESP_OK) {
            free(pixels);
            return err;
        }
        if (src->bytes < required_bytes) {
            free(pixels);
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t i = 0; i + 1 < pixel_count; i += 2) {
            const uint8_t *p = src->data + i * 2;
            int y0 = src->format == LUA_IMAGE_FORMAT_YUYV ? p[0] : p[1];
            int u = src->format == LUA_IMAGE_FORMAT_YUYV ? p[1] : p[0];
            int y1 = src->format == LUA_IMAGE_FORMAT_YUYV ? p[2] : p[3];
            int v = src->format == LUA_IMAGE_FORMAT_YUYV ? p[3] : p[2];
            pixels[i] = lua_image_yuv_to_rgb565(y0, u, v);
            pixels[i + 1] = lua_image_yuv_to_rgb565(y1, u, v);
        }
        if ((pixel_count & 1) != 0) {
            pixels[pixel_count - 1] = pixel_count > 1 ? pixels[pixel_count - 2] : lua_image_rgb888_to_rgb565(0, 0, 0);
        }
        break;
    default:
        free(pixels);
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = lua_image_checked_data_bytes(pixel_count, 2, &required_bytes);
    if (err != ESP_OK) {
        free(pixels);
        return err;
    }
    lua_image_init_owned_view(src, out, (uint8_t *)pixels, required_bytes, LUA_IMAGE_FORMAT_RGB565LE);
    return ESP_OK;
}

static esp_err_t lua_image_require_gray8(const lua_image_source_t *src, lua_image_view_t *out)
{
    size_t pixel_count = 0;
    size_t required_bytes = 0;
    uint8_t *gray = NULL;
    esp_err_t err = lua_image_checked_pixel_count(src->width, src->height, &pixel_count);

    if (err != ESP_OK) {
        return err;
    }

    if (src->format == LUA_IMAGE_FORMAT_GRAY8) {
        lua_image_init_borrowed_view(src, out, LUA_IMAGE_FORMAT_GRAY8);
        return src->bytes >= pixel_count ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    if (src->format == LUA_IMAGE_FORMAT_JPEG || src->format == LUA_IMAGE_FORMAT_MJPEG) {
        lua_image_view_t rgb = {0};
        lua_image_source_t rgb_src = {0};
        err = lua_image_require_rgb565le(src, &rgb);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JPEG gray decode through RGB565 failed: %s", esp_err_to_name(err));
            return err;
        }
        rgb_src.data = rgb.data;
        rgb_src.bytes = rgb.bytes;
        rgb_src.width = rgb.width;
        rgb_src.height = rgb.height;
        rgb_src.format = LUA_IMAGE_FORMAT_RGB565LE;
        strlcpy(rgb_src.source_format, src->source_format, sizeof(rgb_src.source_format));
        err = lua_image_require_gray8(&rgb_src, out);
        lua_image_release_view(&rgb);
        return err;
    }

    gray = (uint8_t *)heap_caps_malloc(pixel_count, MALLOC_CAP_8BIT|MALLOC_CAP_SPIRAM);
    if (!gray) {
        ESP_LOGE(TAG, "motion gray buffer alloc failed: %u bytes", (unsigned)pixel_count);
        return ESP_ERR_NO_MEM;
    }

    switch (src->format) {
    case LUA_IMAGE_FORMAT_RGB565LE:
    case LUA_IMAGE_FORMAT_RGB565BE:
        err = lua_image_checked_data_bytes(pixel_count, 2, &required_bytes);
        if (err != ESP_OK) {
            free(gray);
            return err;
        }
        if (src->bytes < required_bytes) {
            free(gray);
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t i = 0; i < pixel_count; i++) {
            uint16_t pixel = src->format == LUA_IMAGE_FORMAT_RGB565LE ?
                ((uint16_t)src->data[i * 2] | ((uint16_t)src->data[i * 2 + 1] << 8)) :
                (((uint16_t)src->data[i * 2] << 8) | src->data[i * 2 + 1]);
            gray[i] = lua_image_rgb565_to_gray(pixel);
        }
        break;
    case LUA_IMAGE_FORMAT_RGB888:
    case LUA_IMAGE_FORMAT_BGR888:
        err = lua_image_checked_data_bytes(pixel_count, 3, &required_bytes);
        if (err != ESP_OK) {
            free(gray);
            return err;
        }
        if (src->bytes < required_bytes) {
            free(gray);
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t i = 0; i < pixel_count; i++) {
            const uint8_t *p = src->data + i * 3;
            gray[i] = src->format == LUA_IMAGE_FORMAT_RGB888 ?
                lua_image_rgb888_to_gray(p[0], p[1], p[2]) :
                lua_image_rgb888_to_gray(p[2], p[1], p[0]);
        }
        break;
    case LUA_IMAGE_FORMAT_YUYV:
    case LUA_IMAGE_FORMAT_UYVY:
        err = lua_image_checked_data_bytes(pixel_count, 2, &required_bytes);
        if (err != ESP_OK) {
            free(gray);
            return err;
        }
        if (src->bytes < required_bytes) {
            free(gray);
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t i = 0; i + 1 < pixel_count; i += 2) {
            const uint8_t *p = src->data + i * 2;
            gray[i] = src->format == LUA_IMAGE_FORMAT_YUYV ? p[0] : p[1];
            gray[i + 1] = src->format == LUA_IMAGE_FORMAT_YUYV ? p[2] : p[3];
        }
        if ((pixel_count & 1) != 0) {
            gray[pixel_count - 1] = pixel_count > 1 ? gray[pixel_count - 2] : 0;
        }
        break;
    default:
        free(gray);
        return ESP_ERR_NOT_SUPPORTED;
    }

    lua_image_init_owned_view(src, out, gray, pixel_count, LUA_IMAGE_FORMAT_GRAY8);
    return ESP_OK;
}

static esp_err_t lua_image_encode_rgb565le_to_jpeg(const lua_image_view_t *rgb, lua_image_view_t *out)
{
    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    jpeg_enc_handle_t encoder = NULL;
    uint8_t *aligned_input = NULL;
    const uint8_t *input = rgb->data;
    uint8_t *jpeg = NULL;
    size_t pixel_count = 0;
    size_t input_size = 0;
    size_t out_capacity = 0;
    int out_len = 0;
    jpeg_error_t jpeg_err;
    esp_err_t err = lua_image_checked_pixel_count(rgb->width, rgb->height, &pixel_count);

    if (err != ESP_OK) {
        return err;
    }
    ESP_RETURN_ON_ERROR(lua_image_checked_data_bytes(pixel_count, 2, &input_size), TAG, "JPEG encode input size check failed");
    out_capacity = input_size > LUA_IMAGE_MIN_JPEG_BUFFER_SIZE ? input_size : LUA_IMAGE_MIN_JPEG_BUFFER_SIZE;
    if (input_size == 0 || input_size > INT_MAX || out_capacity > INT_MAX) {
        ESP_LOGE(TAG, "JPEG encode invalid size: input=%u output_capacity=%u", (unsigned)input_size, (unsigned)out_capacity);
        return ESP_ERR_INVALID_SIZE;
    }

    if (((uintptr_t)rgb->data & 0x0fU) != 0) {
        aligned_input = (uint8_t *)heap_caps_aligned_alloc(16, input_size, MALLOC_CAP_8BIT|MALLOC_CAP_SPIRAM);
        if (!aligned_input) {
            ESP_LOGE(TAG, "JPEG encode aligned input alloc failed: %u bytes", (unsigned)input_size);
            return ESP_ERR_NO_MEM;
        }
        memcpy(aligned_input, rgb->data, input_size);
        input = aligned_input;
    }

    jpeg = (uint8_t *)heap_caps_malloc(out_capacity, MALLOC_CAP_8BIT|MALLOC_CAP_SPIRAM);
    if (!jpeg) {
        ESP_LOGE(TAG, "JPEG encode output alloc failed: %u bytes", (unsigned)out_capacity);
        free(aligned_input);
        return ESP_ERR_NO_MEM;
    }

    config.width = rgb->width;
    config.height = rgb->height;
    config.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.subsampling = JPEG_SUBSAMPLE_422;
    config.quality = LUA_IMAGE_JPEG_QUALITY;
    jpeg_err = jpeg_enc_open(&config, &encoder);
    if (jpeg_err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_enc_open failed: %d", jpeg_err);
        free(jpeg);
        free(aligned_input);
        return ESP_FAIL;
    }

    jpeg_err = jpeg_enc_process(encoder, (uint8_t *)input, (int)input_size, jpeg, (int)out_capacity, &out_len);
    jpeg_enc_close(encoder);
    free(aligned_input);
    if (jpeg_err != JPEG_ERR_OK || out_len <= 0) {
        ESP_LOGE(TAG, "jpeg_enc_process failed: err=%d out_len=%d", jpeg_err, out_len);
        free(jpeg);
        return ESP_FAIL;
    }

    out->data = jpeg;
    out->bytes = (size_t)out_len;
    out->width = rgb->width;
    out->height = rgb->height;
    out->format = LUA_IMAGE_FORMAT_JPEG;
    out->owned = true;
    strlcpy(out->source_format, rgb->source_format, sizeof(out->source_format));
    return ESP_OK;
}

static esp_err_t lua_image_require_jpeg(const lua_image_source_t *src, lua_image_view_t *out)
{
    lua_image_view_t rgb = {0};
    esp_err_t err;

    if (src->format == LUA_IMAGE_FORMAT_JPEG || src->format == LUA_IMAGE_FORMAT_MJPEG) {
        lua_image_init_borrowed_view(src, out, LUA_IMAGE_FORMAT_JPEG);
        return ESP_OK;
    }

    err = lua_image_require_rgb565le(src, &rgb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode source RGB565 conversion failed: %s", esp_err_to_name(err));
        return err;
    }
    err = lua_image_encode_rgb565le_to_jpeg(&rgb, out);
    lua_image_release_view(&rgb);
    return err;
}

esp_err_t lua_image_convert_view(const lua_image_source_t *src, lua_image_format_t format, lua_image_view_t *out)
{
    size_t pixel_count = 0;

    if (src == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (src->data == NULL || src->bytes == 0 || src->width <= 0 || src->height <= 0) {
        ESP_LOGE(TAG, "invalid source image: %dx%d bytes=%u", src->width, src->height, (unsigned)src->bytes);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(lua_image_checked_pixel_count(src->width, src->height, &pixel_count), TAG, "source image size check failed");

    switch (format) {
    case LUA_IMAGE_FORMAT_RGB565LE:
        return lua_image_require_rgb565le(src, out);
    case LUA_IMAGE_FORMAT_GRAY8:
        return lua_image_require_gray8(src, out);
    case LUA_IMAGE_FORMAT_JPEG:
        return lua_image_require_jpeg(src, out);
    case LUA_IMAGE_FORMAT_MJPEG:
        if (src->format == LUA_IMAGE_FORMAT_JPEG || src->format == LUA_IMAGE_FORMAT_MJPEG) {
            lua_image_init_borrowed_view(src, out, LUA_IMAGE_FORMAT_MJPEG);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "frame format %s cannot be provided as MJPEG", lua_image_format_name(src->format));
        return ESP_ERR_NOT_SUPPORTED;
    default:
        if (src->format == format) {
            lua_image_init_borrowed_view(src, out, format);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "frame format %s cannot be provided as %s", lua_image_format_name(src->format), lua_image_format_name(format));
        return ESP_ERR_NOT_SUPPORTED;
    }
}
