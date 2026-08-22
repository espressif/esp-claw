/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "motion_detect.h"

#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define MOTION_DEFAULT_PIXEL_DIFF_THRESHOLD 24
#define MOTION_DEFAULT_ACTIVE_PIXEL_PERCENT 5
#define MOTION_DEFAULT_CONFIRM_FRAMES 2
#define MOTION_DEFAULT_HOLD_FRAMES 3
#define MOTION_DEFAULT_BLOCK_SIZE 4
#define MOTION_DEFAULT_BLOCK_HIT_PIXELS 12
#define MOTION_DEFAULT_BOX_PADDING 2
#define MOTION_DEFAULT_BOX_DEADBAND 2
#define MOTION_DEFAULT_BOX_SNAP_THRESHOLD 24

static const char *TAG = "motion_detect";

typedef struct {
    uint32_t positive_frames;
    uint32_t hold_frames;
    bool alert_active;
    bool has_box;
    int x1;
    int y1;
    int x2;
    int y2;
} motion_detect_state_t;

struct motion_detect_t {
    motion_detect_config_t buffer_config;
    motion_detect_state_t state;
    uint8_t *prev_luma;
    uint8_t *block_counts;
    size_t prev_luma_size;
    size_t block_count;
    int frame_width;
    int frame_height;
    bool has_previous;
};

static void *motion_alloc(size_t bytes)
{
    void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == nullptr) {
        ptr = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void motion_release_buffers(motion_detect_handle_t handle)
{
    heap_caps_free(handle->prev_luma);
    heap_caps_free(handle->block_counts);
    handle->prev_luma = nullptr;
    handle->block_counts = nullptr;
    handle->prev_luma_size = 0;
    handle->block_count = 0;
    handle->frame_width = 0;
    handle->frame_height = 0;
    handle->has_previous = false;
}

static void motion_reset_state(motion_detect_handle_t handle)
{
    memset(&handle->state, 0, sizeof(handle->state));
    handle->has_previous = false;
    if (handle->prev_luma != nullptr && handle->prev_luma_size > 0) {
        memset(handle->prev_luma, 0, handle->prev_luma_size);
    }
    if (handle->block_counts != nullptr && handle->block_count > 0) {
        memset(handle->block_counts, 0, handle->block_count);
    }
}

static int motion_abs_int(int value)
{
    return value < 0 ? -value : value;
}

static int motion_smooth_box_edge(int current, int target, int deadband, int snap_threshold)
{
    int diff = target - current;

    if (motion_abs_int(diff) <= deadband) {
        return current;
    }
    if (motion_abs_int(diff) >= snap_threshold) {
        return target;
    }
    if (diff > 0) {
        return current + (diff + 1) / 2;
    }
    return current + (diff - 1) / 2;
}

static void motion_update_display_box(motion_detect_handle_t handle,
                                      const motion_detect_result_t *result,
                                      const motion_detect_config_t *config)
{
    if (!handle->state.has_box) {
        handle->state.has_box = true;
        handle->state.x1 = result->raw_x1;
        handle->state.y1 = result->raw_y1;
        handle->state.x2 = result->raw_x2;
        handle->state.y2 = result->raw_y2;
        return;
    }

    handle->state.x1 = motion_smooth_box_edge(handle->state.x1, result->raw_x1,
                                               config->box_deadband, config->box_snap_threshold);
    handle->state.y1 = motion_smooth_box_edge(handle->state.y1, result->raw_y1,
                                               config->box_deadband, config->box_snap_threshold);
    handle->state.x2 = motion_smooth_box_edge(handle->state.x2, result->raw_x2,
                                               config->box_deadband, config->box_snap_threshold);
    handle->state.y2 = motion_smooth_box_edge(handle->state.y2, result->raw_y2,
                                               config->box_deadband, config->box_snap_threshold);
}

static bool motion_state_update(motion_detect_handle_t handle,
                                bool detected,
                                const motion_detect_config_t *config,
                                motion_detect_event_t *event_out)
{
    bool alert_active = false;
    *event_out = MOTION_DETECT_EVENT_NONE;

    if (detected) {
        if (handle->state.positive_frames < (uint32_t)config->confirm_frames) {
            handle->state.positive_frames++;
        }
        if (handle->state.positive_frames >= (uint32_t)config->confirm_frames) {
            handle->state.hold_frames = (uint32_t)config->hold_frames;
            alert_active = true;
        }
    } else {
        handle->state.positive_frames = 0;
        if (handle->state.hold_frames > 0) {
            handle->state.hold_frames--;
            alert_active = true;
        }
    }

    if (alert_active != handle->state.alert_active) {
        *event_out = alert_active ? MOTION_DETECT_EVENT_ACTIVATED : MOTION_DETECT_EVENT_CLEARED;
    }
    handle->state.alert_active = alert_active;
    return alert_active;
}

static inline uint8_t motion_rgb565_to_luma8(uint16_t pixel)
{
    uint8_t g6 = (uint8_t)((pixel >> 5) & 0x3f);
    return (uint8_t)((g6 << 2) | (g6 >> 4));
}

static void motion_store_luma_roi(const uint16_t *src,
                                  uint8_t *dst,
                                  int frame_width,
                                  const motion_detect_config_t *config)
{
    for (int y = 0; y < config->roi_height; y++) {
        size_t frame_row = (size_t)(config->roi_y + y) * (size_t)frame_width + (size_t)config->roi_x;
        size_t roi_row = (size_t)y * (size_t)config->roi_width;
        for (int x = 0; x < config->roi_width; x++) {
            dst[roi_row + (size_t)x] = motion_rgb565_to_luma8(src[frame_row + (size_t)x]);
        }
    }
}

static void motion_compare_frame(motion_detect_handle_t handle,
                                 const uint16_t *current,
                                 const motion_detect_config_t *config,
                                 motion_detect_result_t *result)
{
    const int blocks_x = (config->roi_width + config->block_size - 1) / config->block_size;
    const int blocks_y = (config->roi_height + config->block_size - 1) / config->block_size;
    const uint32_t total = (uint32_t)config->roi_width * (uint32_t)config->roi_height;
    uint32_t threshold = (uint32_t)(((uint64_t)total * (uint32_t)config->active_pixel_percent) / 100U);

    if (threshold == 0) {
        threshold = 1;
    }
    result->threshold_pixels = threshold;
    memset(handle->block_counts, 0, handle->block_count);

    result->raw_x1 = config->roi_x + config->roi_width - 1;
    result->raw_y1 = config->roi_y + config->roi_height - 1;
    result->raw_x2 = config->roi_x;
    result->raw_y2 = config->roi_y;

    for (int y = 0; y < config->roi_height; y++) {
        size_t frame_row = (size_t)(config->roi_y + y) * (size_t)handle->frame_width + (size_t)config->roi_x;
        size_t roi_row = (size_t)y * (size_t)config->roi_width;
        size_t block_row = (size_t)(y / config->block_size) * (size_t)blocks_x;
        for (int x = 0; x < config->roi_width; x++) {
            size_t roi_index = roi_row + (size_t)x;
            uint8_t luma = motion_rgb565_to_luma8(current[frame_row + (size_t)x]);
            int diff = abs((int)luma - (int)handle->prev_luma[roi_index]);
            handle->prev_luma[roi_index] = luma;

            if (diff > config->pixel_diff_threshold) {
                size_t block_index = block_row + (size_t)(x / config->block_size);
                if (handle->block_counts[block_index] < UINT8_MAX) {
                    handle->block_counts[block_index]++;
                }
            }
        }
    }

    for (int by = 0; by < blocks_y; by++) {
        int block_y = by * config->block_size;
        int block_h = config->roi_height - block_y;
        if (block_h > config->block_size) {
            block_h = config->block_size;
        }
        for (int bx = 0; bx < blocks_x; bx++) {
            int block_x = bx * config->block_size;
            int block_w = config->roi_width - block_x;
            if (block_w > config->block_size) {
                block_w = config->block_size;
            }
            if (handle->block_counts[(size_t)by * (size_t)blocks_x + (size_t)bx] < config->block_hit_pixels) {
                continue;
            }

            result->active_pixels += (uint32_t)block_w * (uint32_t)block_h;
            result->has_raw_box = true;
            int block_x1 = config->roi_x + block_x;
            int block_y1 = config->roi_y + block_y;
            int block_x2 = block_x1 + block_w - 1;
            int block_y2 = block_y1 + block_h - 1;
            if (block_x1 < result->raw_x1) {
                result->raw_x1 = block_x1;
            }
            if (block_y1 < result->raw_y1) {
                result->raw_y1 = block_y1;
            }
            if (block_x2 > result->raw_x2) {
                result->raw_x2 = block_x2;
            }
            if (block_y2 > result->raw_y2) {
                result->raw_y2 = block_y2;
            }
        }
    }

    result->detected = result->active_pixels > threshold;
    if (!result->detected || !result->has_raw_box) {
        result->has_raw_box = false;
        return;
    }

    result->raw_x1 -= config->box_padding;
    result->raw_y1 -= config->box_padding;
    result->raw_x2 += config->box_padding;
    result->raw_y2 += config->box_padding;
    if (result->raw_x1 < config->roi_x) {
        result->raw_x1 = config->roi_x;
    }
    if (result->raw_y1 < config->roi_y) {
        result->raw_y1 = config->roi_y;
    }
    if (result->raw_x2 >= config->roi_x + config->roi_width) {
        result->raw_x2 = config->roi_x + config->roi_width - 1;
    }
    if (result->raw_y2 >= config->roi_y + config->roi_height) {
        result->raw_y2 = config->roi_y + config->roi_height - 1;
    }
}

static esp_err_t motion_normalize_config(motion_detect_config_t *config, int frame_width, int frame_height)
{
    if (config->roi_width <= 0) {
        config->roi_width = frame_width;
    }
    if (config->roi_height <= 0) {
        config->roi_height = frame_height;
    }
    if (config->roi_x < 0 || config->roi_y < 0 || config->roi_width <= 0 || config->roi_height <= 0 ||
        config->roi_x > frame_width - config->roi_width || config->roi_y > frame_height - config->roi_height ||
        config->block_hit_pixels > config->block_size * config->block_size) {
        ESP_LOGE(TAG, "invalid config: roi=%d,%d %dx%d frame=%dx%d block_size=%d block_hit_pixels=%d",
                 config->roi_x, config->roi_y, config->roi_width, config->roi_height, frame_width, frame_height,
                 config->block_size, config->block_hit_pixels);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool motion_buffers_match(motion_detect_handle_t handle,
                                 const motion_detect_config_t *config,
                                 int frame_width,
                                 int frame_height)
{
    return handle->prev_luma != nullptr && handle->block_counts != nullptr &&
           handle->frame_width == frame_width && handle->frame_height == frame_height &&
           handle->buffer_config.roi_x == config->roi_x &&
           handle->buffer_config.roi_y == config->roi_y &&
           handle->buffer_config.roi_width == config->roi_width &&
           handle->buffer_config.roi_height == config->roi_height &&
           handle->buffer_config.block_size == config->block_size;
}

static esp_err_t motion_prepare(motion_detect_handle_t handle,
                                const motion_detect_config_t *config,
                                int frame_width,
                                int frame_height)
{
    size_t luma_size = (size_t)config->roi_width * (size_t)config->roi_height;
    size_t blocks_x = (size_t)((config->roi_width + config->block_size - 1) / config->block_size);
    size_t blocks_y = (size_t)((config->roi_height + config->block_size - 1) / config->block_size);
    size_t block_count = blocks_x * blocks_y;

    if (motion_buffers_match(handle, config, frame_width, frame_height)) {
        handle->buffer_config = *config;
        return ESP_OK;
    }

    motion_release_buffers(handle);
    handle->prev_luma = (uint8_t *)motion_alloc(luma_size);
    handle->block_counts = (uint8_t *)motion_alloc(block_count);
    if (handle->prev_luma == nullptr || handle->block_counts == nullptr) {
        motion_release_buffers(handle);
        ESP_LOGE(TAG, "alloc buffers failed: luma=%zu blocks=%zu", luma_size, block_count);
        return ESP_ERR_NO_MEM;
    }
    handle->prev_luma_size = luma_size;
    handle->block_count = block_count;
    handle->frame_width = frame_width;
    handle->frame_height = frame_height;
    handle->buffer_config = *config;
    motion_reset_state(handle);
    return ESP_OK;
}

extern "C" void motion_detect_config_set_defaults(motion_detect_config_t *config)
{
    if (config == nullptr) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->pixel_diff_threshold = MOTION_DEFAULT_PIXEL_DIFF_THRESHOLD;
    config->active_pixel_percent = MOTION_DEFAULT_ACTIVE_PIXEL_PERCENT;
    config->confirm_frames = MOTION_DEFAULT_CONFIRM_FRAMES;
    config->hold_frames = MOTION_DEFAULT_HOLD_FRAMES;
    config->block_size = MOTION_DEFAULT_BLOCK_SIZE;
    config->block_hit_pixels = MOTION_DEFAULT_BLOCK_HIT_PIXELS;
    config->box_padding = MOTION_DEFAULT_BOX_PADDING;
    config->box_deadband = MOTION_DEFAULT_BOX_DEADBAND;
    config->box_snap_threshold = MOTION_DEFAULT_BOX_SNAP_THRESHOLD;
}

extern "C" esp_err_t motion_detect_create(motion_detect_handle_t *ret_handle)
{
    if (ret_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_handle = (motion_detect_handle_t)calloc(1, sizeof(struct motion_detect_t));
    if (*ret_handle == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

extern "C" void motion_detect_delete(motion_detect_handle_t handle)
{
    if (handle == nullptr) {
        return;
    }
    motion_release_buffers(handle);
    free(handle);
}

extern "C" void motion_detect_reset(motion_detect_handle_t handle)
{
    if (handle != nullptr) {
        motion_reset_state(handle);
    }
}

extern "C" void motion_detect_close(motion_detect_handle_t handle)
{
    if (handle != nullptr) {
        motion_release_buffers(handle);
        memset(&handle->state, 0, sizeof(handle->state));
    }
}

extern "C" esp_err_t motion_detect_process_rgb565(motion_detect_handle_t handle,
                                                    const uint8_t *data,
                                                    size_t bytes,
                                                    int width,
                                                    int height,
                                                    const motion_detect_config_t *input_config,
                                                    motion_detect_result_t *out_result)
{
    if (handle == nullptr || data == nullptr || input_config == nullptr || out_result == nullptr ||
        width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX ||
        (size_t)width > SIZE_MAX / (size_t)height ||
        (size_t)width * (size_t)height > SIZE_MAX / sizeof(uint16_t) ||
        bytes < (size_t)width * (size_t)height * sizeof(uint16_t)) {
        ESP_LOGE(TAG, "invalid process input: handle=%p data=%p config=%p out=%p size=%dx%d bytes=%zu",
                 (void *)handle, (void *)data, (void *)input_config, (void *)out_result, width, height, bytes);
        return ESP_ERR_INVALID_ARG;
    }

    motion_detect_config_t config = *input_config;
    esp_err_t err = motion_normalize_config(&config, width, height);
    if (err != ESP_OK || (uint64_t)config.roi_width * (uint64_t)config.roi_height > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    err = motion_prepare(handle, &config, width, height);
    if (err != ESP_OK) {
        return err;
    }

    memset(out_result, 0, sizeof(*out_result));
    out_result->has_previous = handle->has_previous;
    out_result->roi_x = config.roi_x;
    out_result->roi_y = config.roi_y;
    out_result->roi_width = config.roi_width;
    out_result->roi_height = config.roi_height;
    const uint16_t *pixels = (const uint16_t *)data;

    if (!handle->has_previous) {
        motion_store_luma_roi(pixels, handle->prev_luma, width, &config);
        handle->has_previous = true;
        uint32_t total = (uint32_t)config.roi_width * (uint32_t)config.roi_height;
        out_result->threshold_pixels = (uint32_t)(((uint64_t)total * (uint32_t)config.active_pixel_percent) / 100U);
        if (out_result->threshold_pixels == 0) {
            out_result->threshold_pixels = 1;
        }
    } else {
        motion_compare_frame(handle, pixels, &config, out_result);
        out_result->alert_active = motion_state_update(handle, out_result->detected, &config, &out_result->event);
        if (out_result->detected && out_result->has_raw_box) {
            motion_update_display_box(handle, out_result, &config);
        } else if (out_result->event == MOTION_DETECT_EVENT_CLEARED) {
            handle->state.has_box = false;
        }
    }

    out_result->positive_frames = handle->state.positive_frames;
    out_result->hold_frames = handle->state.hold_frames;
    out_result->alert_active = handle->state.alert_active;
    out_result->has_display_box = handle->state.has_box;
    if (handle->state.has_box) {
        out_result->display_x1 = handle->state.x1;
        out_result->display_y1 = handle->state.y1;
        out_result->display_x2 = handle->state.x2;
        out_result->display_y2 = handle->state.y2;
    }
    return ESP_OK;
}
