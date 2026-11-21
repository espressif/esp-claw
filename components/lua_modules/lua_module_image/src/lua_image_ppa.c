/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_image_ppa.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>

#include "soc/soc_caps.h"

#if SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define LUA_IMAGE_PPA_ALIGNMENT 64U
static const char *TAG = "image_ppa";

static _lock_t s_ppa_lock;
static ppa_client_handle_t s_ppa_client;
static bool s_ppa_disabled;

static esp_err_t lua_image_ppa_init_locked(void)
{
    if (s_ppa_client != NULL) {
        return ESP_OK;
    }
    if (s_ppa_disabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const ppa_client_config_t config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t err = ppa_register_client(&config, &s_ppa_client);
    if (err != ESP_OK) {
        s_ppa_disabled = true;
        ESP_LOGE(TAG, "PPA client init failed: %s", esp_err_to_name(err));
    }
    return err;
}

static bool lua_image_ppa_input_config(lua_image_format_t format, ppa_srm_color_mode_t *color_mode, bool *rgb_swap, bool *byte_swap,
                                       size_t *bytes_per_pixel)
{
    *rgb_swap = false;
    *byte_swap = false;
    switch (format) {
    case LUA_IMAGE_FORMAT_RGB565LE:
        *color_mode = PPA_SRM_COLOR_MODE_RGB565;
        *bytes_per_pixel = 2;
        return true;
    case LUA_IMAGE_FORMAT_RGB565BE:
        *color_mode = PPA_SRM_COLOR_MODE_RGB565;
        *byte_swap = true;
        *bytes_per_pixel = 2;
        return true;
    case LUA_IMAGE_FORMAT_RGB888:
        *color_mode = PPA_SRM_COLOR_MODE_RGB888;
        *rgb_swap = true;
        *bytes_per_pixel = 3;
        return true;
    case LUA_IMAGE_FORMAT_BGR888:
        *color_mode = PPA_SRM_COLOR_MODE_RGB888;
        *bytes_per_pixel = 3;
        return true;
    case LUA_IMAGE_FORMAT_GRAY8:
        *color_mode = PPA_SRM_COLOR_MODE_GRAY8;
        *bytes_per_pixel = 1;
        return true;
    case LUA_IMAGE_FORMAT_YUYV:
        *color_mode = PPA_SRM_COLOR_MODE_YUV422_YUYV;
        *bytes_per_pixel = 2;
        return true;
    case LUA_IMAGE_FORMAT_UYVY:
        *color_mode = PPA_SRM_COLOR_MODE_YUV422_UYVY;
        *bytes_per_pixel = 2;
        return true;
    default:
        return false;
    }
}

static bool lua_image_ppa_output_config(lua_image_format_t format, ppa_srm_color_mode_t *color_mode, size_t *bytes_per_pixel)
{
    switch (format) {
    case LUA_IMAGE_FORMAT_RGB565LE:
        *color_mode = PPA_SRM_COLOR_MODE_RGB565;
        *bytes_per_pixel = 2;
        return true;
    case LUA_IMAGE_FORMAT_BGR888:
        *color_mode = PPA_SRM_COLOR_MODE_RGB888;
        *bytes_per_pixel = 3;
        return true;
    case LUA_IMAGE_FORMAT_GRAY8:
        *color_mode = PPA_SRM_COLOR_MODE_GRAY8;
        *bytes_per_pixel = 1;
        return true;
    default:
        return false;
    }
}

static bool lua_image_ppa_scale(int source, int target, uint16_t *scale_q4)
{
    uint64_t scaled_target = (uint64_t)(uint32_t)target * 16U;
    uint64_t scale = scaled_target / (uint32_t)source;
    if (scaled_target % (uint32_t)source != 0 || scale == 0 || scale >= 4096U) {
        return false;
    }
    *scale_q4 = (uint16_t)scale;
    return true;
}

esp_err_t lua_image_ppa_transform(const lua_image_source_t *src, lua_image_format_t dst_format, int dst_width, int dst_height,
                                  lua_image_view_t *out)
{
    ppa_srm_color_mode_t in_mode;
    ppa_srm_color_mode_t out_mode;
    size_t in_bpp;
    size_t out_bpp;
    bool rgb_swap;
    bool byte_swap;
    uint16_t scale_x;
    uint16_t scale_y;
    size_t output_bytes;
    size_t allocated_bytes;
    uint8_t *output;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (src == NULL || src->data == NULL || src->width <= 0 || src->height <= 0 || dst_width <= 0 || dst_height <= 0 ||
        !lua_image_ppa_input_config(src->format, &in_mode, &rgb_swap, &byte_swap, &in_bpp) ||
        !lua_image_ppa_output_config(dst_format, &out_mode, &out_bpp)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* RGB-to-gray coefficients are global PPA state, so only gray pass-through is safe here. */
    if (dst_format == LUA_IMAGE_FORMAT_GRAY8 && src->format != LUA_IMAGE_FORMAT_GRAY8) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (((src->format == LUA_IMAGE_FORMAT_UYVY || src->format == LUA_IMAGE_FORMAT_YUYV) && (src->width & 1) != 0) ||
        !lua_image_ppa_scale(src->width, dst_width, &scale_x) || !lua_image_ppa_scale(src->height, dst_height, &scale_y)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((size_t)src->width > SIZE_MAX / (size_t)src->height / in_bpp || (size_t)dst_width > SIZE_MAX / (size_t)dst_height / out_bpp) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t input_bytes = (size_t)src->width * (size_t)src->height * in_bpp;
    output_bytes = (size_t)dst_width * (size_t)dst_height * out_bpp;
    if (input_bytes > UINT32_MAX || output_bytes > UINT32_MAX - (LUA_IMAGE_PPA_ALIGNMENT - 1) || src->bytes < input_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    allocated_bytes = (output_bytes + LUA_IMAGE_PPA_ALIGNMENT - 1) & ~(LUA_IMAGE_PPA_ALIGNMENT - 1);
    output = NULL;
    _lock_acquire(&s_ppa_lock);
    esp_err_t err = lua_image_ppa_init_locked();
    if (err == ESP_OK) {
        output = (uint8_t *)heap_caps_aligned_alloc(LUA_IMAGE_PPA_ALIGNMENT, allocated_bytes,
                                                    MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        err = output != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        const ppa_srm_oper_config_t config = {
            .in = {
                .buffer = src->data,
                .pic_w = (uint32_t)src->width,
                .pic_h = (uint32_t)src->height,
                .block_w = (uint32_t)src->width,
                .block_h = (uint32_t)src->height,
                .srm_cm = in_mode,
                .yuv_range = PPA_COLOR_RANGE_LIMIT,
                .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
            },
            .out = {
                .buffer = output,
                .buffer_size = (uint32_t)allocated_bytes,
                .pic_w = (uint32_t)dst_width,
                .pic_h = (uint32_t)dst_height,
                .srm_cm = out_mode,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = (float)scale_x / 16.0f,
            .scale_y = (float)scale_y / 16.0f,
            .rgb_swap = rgb_swap,
            .byte_swap = byte_swap,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        err = ppa_do_scale_rotate_mirror(s_ppa_client, &config);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PPA transform failed, using CPU: %s", esp_err_to_name(err));
        free(output);
    } else {
        out->data = output;
        out->bytes = output_bytes;
        out->width = dst_width;
        out->height = dst_height;
        out->format = dst_format;
        out->owned = true;
        strlcpy(out->source_format, src->source_format, sizeof(out->source_format));
    }
    _lock_release(&s_ppa_lock);
    return err;
}
#else
esp_err_t lua_image_ppa_transform(const lua_image_source_t *src, lua_image_format_t dst_format, int dst_width, int dst_height,
                                  lua_image_view_t *out)
{
    (void)src;
    (void)dst_format;
    (void)dst_width;
    (void)dst_height;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
