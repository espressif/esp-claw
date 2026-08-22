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
    int roi_x;
    int roi_y;
    int roi_width;
    int roi_height;
    int pixel_diff_threshold;
    int active_pixel_percent;
    int confirm_frames;
    int hold_frames;
    int block_size;
    int block_hit_pixels;
    int box_padding;
    int box_deadband;
    int box_snap_threshold;
} motion_detect_config_t;

typedef enum {
    MOTION_DETECT_EVENT_NONE = 0,
    MOTION_DETECT_EVENT_ACTIVATED,
    MOTION_DETECT_EVENT_CLEARED,
} motion_detect_event_t;

typedef struct {
    bool has_previous;
    bool detected;
    bool alert_active;
    motion_detect_event_t event;
    uint32_t active_pixels;
    uint32_t threshold_pixels;
    uint32_t positive_frames;
    uint32_t hold_frames;
    int roi_x;
    int roi_y;
    int roi_width;
    int roi_height;
    bool has_raw_box;
    int raw_x1;
    int raw_y1;
    int raw_x2;
    int raw_y2;
    bool has_display_box;
    int display_x1;
    int display_y1;
    int display_x2;
    int display_y2;
} motion_detect_result_t;

typedef struct motion_detect_t *motion_detect_handle_t;

void motion_detect_config_set_defaults(motion_detect_config_t *config);
esp_err_t motion_detect_create(motion_detect_handle_t *ret_handle);
void motion_detect_delete(motion_detect_handle_t handle);
void motion_detect_reset(motion_detect_handle_t handle);
void motion_detect_close(motion_detect_handle_t handle);
esp_err_t motion_detect_process_rgb565(motion_detect_handle_t handle,
                                       const uint8_t *data,
                                       size_t bytes,
                                       int width,
                                       int height,
                                       const motion_detect_config_t *config,
                                       motion_detect_result_t *out_result);

#ifdef __cplusplus
}
#endif
