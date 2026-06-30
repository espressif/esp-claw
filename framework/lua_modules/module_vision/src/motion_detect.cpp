/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "motion_detect.h"

#include <cstdlib>
#include <cstring>

#include "esp_log.h"

static const char *TAG = "motion";

namespace dl {
namespace image {

static inline int abs_int(int value)
{
    return value < 0 ? -value : value;
}

int img_t::col_step() const
{
    return pix_type == DL_IMAGE_PIX_TYPE_GRAY ? 1 : 0;
}

uint32_t get_moving_point_number(const img_t &img1, const img_t &img2, int stride, uint8_t threshold)
{
    if (img1.pix_type != DL_IMAGE_PIX_TYPE_GRAY || img2.pix_type != DL_IMAGE_PIX_TYPE_GRAY || img1.width != img2.width || img1.height != img2.height) {
        ESP_LOGE(TAG,
                 "Invalid motion frame metadata: pix_type=%d/%d size=%ux%u/%ux%u",
                 img1.pix_type,
                 img2.pix_type,
                 (unsigned)img1.width,
                 (unsigned)img1.height,
                 (unsigned)img2.width,
                 (unsigned)img2.height);
        return UINT32_MAX;
    }
    if (img1.data == nullptr || img2.data == nullptr || stride <= 0) {
        ESP_LOGE(TAG, "Invalid motion input: data=%p/%p stride=%d", img1.data, img2.data, stride);
        return UINT32_MAX;
    }

    const uint8_t *data1 = (const uint8_t *)img1.data;
    const uint8_t *data2 = (const uint8_t *)img2.data;
    uint32_t n_moving_pts = 0;
    int width = img1.width;
    int height = img1.height;

    // Compare only gray frames. Source-format conversion belongs to lua_image, not to the detector.
    for (int y = 0; y < height; y += stride) {
        size_t row = (size_t)y * (size_t)width;
        for (int x = 0; x < width; x += stride) {
            size_t index = row + (size_t)x;
            if (abs_int((int)data1[index] - (int)data2[index]) > threshold) {
                n_moving_pts++;
            }
        }
    }
    return n_moving_pts;
}

} // namespace image
} // namespace dl

static uint32_t motion_detect_sample_points(const motion_detect_gray_frame_t *frame, int stride)
{
    uint32_t cols = (frame->width + (uint32_t)stride - 1) / (uint32_t)stride;
    uint32_t rows = (frame->height + (uint32_t)stride - 1) / (uint32_t)stride;
    if (rows != 0 && cols > UINT32_MAX / rows) {
        ESP_LOGE(TAG, "motion sample count overflow: cols=%u rows=%u", (unsigned)cols, (unsigned)rows);
        return UINT32_MAX;
    }
    return cols * rows;
}

static uint8_t motion_detect_pixel_threshold_to_u8(double threshold)
{
    int value = (int)(threshold * 255.0 + 0.5);
    if (value < 0) {
        value = 0;
    } else if (value > 255) {
        value = 255;
    }
    return (uint8_t)value;
}

static dl::image::img_t motion_detect_to_img(const motion_detect_gray_frame_t *frame)
{
    dl::image::img_t img;
    img.data = (void *)frame->data;
    img.width = (uint16_t)frame->width;
    img.height = (uint16_t)frame->height;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_GRAY;
    return img;
}

static bool motion_detect_validate_gray_frame(const motion_detect_gray_frame_t *frame, const char *name)
{
    if (frame == nullptr || frame->data == nullptr || frame->bytes == 0) {
        ESP_LOGE(TAG, "invalid %s frame data", name);
        return false;
    }
    if (frame->width == 0 || frame->height == 0 || frame->width > UINT16_MAX || frame->height > UINT16_MAX) {
        ESP_LOGE(TAG, "invalid %s frame size: %ux%u", name, (unsigned)frame->width, (unsigned)frame->height);
        return false;
    }
    if ((size_t)frame->width > SIZE_MAX / (size_t)frame->height || frame->bytes < (size_t)frame->width * (size_t)frame->height) {
        ESP_LOGE(TAG, "%s frame payload too small: size=%ux%u bytes=%u", name, (unsigned)frame->width, (unsigned)frame->height, (unsigned)frame->bytes);
        return false;
    }
    return true;
}

extern "C" void motion_detect_context_reset(motion_detect_context_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    free(ctx->prev_data);
    memset(ctx, 0, sizeof(*ctx));
}

extern "C" bool motion_detect_context_has_previous(const motion_detect_context_t *ctx, const motion_detect_gray_frame_t *frame)
{
    return ctx != nullptr &&
           frame != nullptr &&
           ctx->prev_data != nullptr &&
           ctx->prev_frame.width == frame->width &&
           ctx->prev_frame.height == frame->height &&
           ctx->prev_frame.bytes == frame->bytes;
}

extern "C" esp_err_t motion_detect_context_update(motion_detect_context_t *ctx, const motion_detect_gray_frame_t *frame)
{
    if (!motion_detect_validate_gray_frame(frame, "previous")) {
        ESP_LOGE(TAG, "invalid previous-frame update input");
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->prev_capacity != frame->bytes) {
        uint8_t *new_frame = (uint8_t *)malloc(frame->bytes);
        if (new_frame == nullptr) {
            ESP_LOGE(TAG, "failed to allocate previous motion frame: %u bytes", (unsigned)frame->bytes);
            return ESP_ERR_NO_MEM;
        }
        // Swap owned storage only after allocation succeeds, so the old copy remains valid on allocation failure.
        free(ctx->prev_data);
        ctx->prev_data = new_frame;
        ctx->prev_capacity = frame->bytes;
    }
    memcpy(ctx->prev_data, frame->data, frame->bytes);
    ctx->prev_frame.data = ctx->prev_data;
    ctx->prev_frame.width = frame->width;
    ctx->prev_frame.height = frame->height;
    ctx->prev_frame.bytes = frame->bytes;
    return ESP_OK;
}

extern "C" esp_err_t motion_detect_compare_gray(const motion_detect_gray_frame_t *previous,
                                                const motion_detect_gray_frame_t *current,
                                                const motion_detect_config_t *config,
                                                motion_detect_result_t *out)
{
    if (previous == nullptr || current == nullptr || config == nullptr || out == nullptr) {
        ESP_LOGE(TAG, "invalid compare input pointer");
        return ESP_ERR_INVALID_ARG;
    }
    if (!motion_detect_validate_gray_frame(previous, "previous") || !motion_detect_validate_gray_frame(current, "current")) {
        return ESP_ERR_INVALID_ARG;
    }
    if (previous->width != current->width || previous->height != current->height || previous->bytes != current->bytes) {
        ESP_LOGE(TAG, "motion frame mismatch: %ux%u/%u vs %ux%u/%u",
                 (unsigned)previous->width, (unsigned)previous->height, (unsigned)previous->bytes,
                 (unsigned)current->width, (unsigned)current->height, (unsigned)current->bytes);
        return ESP_ERR_INVALID_SIZE;
    }
    if (config->stride <= 0 || config->pixel_threshold < 0.0 || config->pixel_threshold > 1.0 ||
        (config->has_moving_threshold && (config->moving_threshold < 0.0 || config->moving_threshold > 1.0))) {
        ESP_LOGE(TAG, "invalid motion config: stride=%d pixel_threshold=%f moving_threshold=%f has_moving_threshold=%d",
                 config->stride, config->pixel_threshold, config->moving_threshold, config->has_moving_threshold);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t pixel_threshold_u8 = motion_detect_pixel_threshold_to_u8(config->pixel_threshold);
    uint32_t sample_points = motion_detect_sample_points(current, config->stride);
    if (sample_points == UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t moving_points = dl::image::get_moving_point_number(motion_detect_to_img(previous),
                                                                motion_detect_to_img(current),
                                                                config->stride,
                                                                pixel_threshold_u8);
    if (moving_points == UINT32_MAX) {
        return ESP_FAIL;
    }

    memset(out, 0, sizeof(*out));
    out->moving_points = moving_points;
    out->sample_points = sample_points;
    out->moving_ratio = sample_points > 0 ? (double)moving_points / (double)sample_points : 0.0;
    out->moved = config->has_moving_threshold && out->moving_ratio > config->moving_threshold;
    return ESP_OK;
}
