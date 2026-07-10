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

typedef struct {
    const uint8_t *data; ///< Borrowed GRAY8 frame data
    uint32_t width;     ///< Frame width in pixels
    uint32_t height;    ///< Frame height in pixels
    size_t bytes;       ///< Frame payload size in bytes
} motion_detect_gray_frame_t;

typedef struct {
    int stride;                 ///< Pixel sampling interval
    double pixel_threshold;     ///< Per-sample gray-value ratio threshold in [0, 1]
    double moving_threshold;    ///< Moving sample ratio threshold in [0, 1]
    bool has_moving_threshold;  ///< True when moving_threshold should set result.moved
} motion_detect_config_t;

typedef struct {
    uint32_t moving_points; ///< Number of changed sample points
    uint32_t sample_points; ///< Total sampled points
    double moving_ratio;    ///< moving_points / sample_points
    bool moved;             ///< True when moving_ratio > moving_threshold
} motion_detect_result_t;

typedef struct {
    uint8_t *prev_data;                      ///< Owned previous GRAY8 frame copy
    size_t prev_capacity;                    ///< Allocated previous-frame byte capacity
    motion_detect_gray_frame_t prev_frame;   ///< Metadata for prev_data
} motion_detect_context_t;

void motion_detect_context_reset(motion_detect_context_t *ctx);
bool motion_detect_context_has_previous(const motion_detect_context_t *ctx, const motion_detect_gray_frame_t *frame);
esp_err_t motion_detect_context_update(motion_detect_context_t *ctx, const motion_detect_gray_frame_t *frame);
esp_err_t motion_detect_compare_gray(const motion_detect_gray_frame_t *previous,
                                     const motion_detect_gray_frame_t *current,
                                     const motion_detect_config_t *config,
                                     motion_detect_result_t *out);

#ifdef __cplusplus
}

namespace dl {
namespace image {

typedef enum {
    DL_IMAGE_PIX_TYPE_GRAY = 0, ///< 8-bit gray pixel
} pix_type_t;

/**
 * @brief Lightweight image view used by the motion detector.
 *
 * The detector never owns the image data. Callers must keep data valid for the duration of get_moving_point_number().
 */
struct img_t {
    void *data;          ///< Image data pointer
    uint16_t width;      ///< Image width in pixels
    uint16_t height;     ///< Image height in pixels
    pix_type_t pix_type; ///< Image pixel format

    /**
     * @brief Get bytes per pixel column step for the current pixel format.
     *
     * @return Bytes per pixel, or 0 if the pixel format is unsupported
     */
    int col_step() const;
};

/**
 * @brief Detect motion by counting sample points whose gray value changes above threshold.
 *
 * @param img1       Previous image view
 * @param img2       Current image view
 * @param stride     Pixel sampling stride
 * @param threshold  Per-sample gray-value activation threshold
 *
 * @return Activated sample point count, or UINT32_MAX on invalid input
 */
uint32_t get_moving_point_number(const img_t &img1, const img_t &img2, const int stride, const uint8_t threshold = 5);

} // namespace image
} // namespace dl
#endif
