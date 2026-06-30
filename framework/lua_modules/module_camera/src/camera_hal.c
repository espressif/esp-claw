/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "camera_hal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "linux/videodev2.h"

#define CAMERA_DEFAULT_TIMEOUT_MS   5000
#define CAMERA_SETTLE_TIMEOUT_MS   30000  /* settle at open time, allow slow SPI sensors */
#define CAMERA_BUFFER_COUNT            3
#define CAMERA_STREAM_SETTLE_FRAMES    3  /* reduced: 3 frames enough for AE/AWB stabilize */

static const char *TAG = "camera_service";

static esp_err_t camera_settle_stream_locked(int64_t deadline_us);
static void camera_fourcc_to_string(uint32_t pixel_format, char out[5]);
static esp_err_t camera_requeue_buffer_locked(struct v4l2_buffer *buffer);

typedef struct {
    bool active;
    struct v4l2_buffer buffer;
    uint8_t *ptr;
    size_t bytes;
} camera_borrowed_frame_t;

typedef struct {
    SemaphoreHandle_t lock;
    bool opened;
    bool streaming;
    bool close_pending;
    uint32_t settled_frames;
    int fd;
    uint8_t *buffers[CAMERA_BUFFER_COUNT];
    size_t buffer_lengths[CAMERA_BUFFER_COUNT];
    uint32_t buffer_count;
    camera_borrowed_frame_t borrowed[CAMERA_BUFFER_COUNT];
    uint32_t borrowed_count;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
} camera_state_t;

static StaticSemaphore_t s_camera_lock_buf;
static camera_state_t s_camera = {
    .fd = -1,
};

static esp_err_t camera_lock(void)
{
    if (s_camera.lock == NULL) {
        s_camera.lock = xSemaphoreCreateMutexStatic(&s_camera_lock_buf);
    }

    if (xSemaphoreTake(s_camera.lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Camera mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void camera_unlock(void)
{
    if (s_camera.lock != NULL) {
        xSemaphoreGive(s_camera.lock);
    }
}

static inline int camera_effective_timeout_ms(int timeout_ms)
{
    return timeout_ms > 0 ? timeout_ms : CAMERA_DEFAULT_TIMEOUT_MS;
}

static int camera_remaining_timeout_ms(int64_t deadline_us)
{
    int64_t remaining_us = deadline_us - esp_timer_get_time();

    if (remaining_us <= 0) {
        return 0;
    }
    if (remaining_us > INT32_MAX * 1000LL) {
        return INT32_MAX;
    }
    return (int)((remaining_us + 999LL) / 1000LL);
}

static void camera_fourcc_to_string(uint32_t pixel_format, char out[5])
{
    out[0] = (char)(pixel_format & 0xff);
    out[1] = (char)((pixel_format >> 8) & 0xff);
    out[2] = (char)((pixel_format >> 16) & 0xff);
    out[3] = (char)((pixel_format >> 24) & 0xff);
    out[4] = '\0';

    for (int i = 0; i < 4; ++i) {
        if (out[i] < 32 || out[i] > 126) {
            out[i] = '.';
        }
    }
}

static void camera_apply_internal_sizeimage(struct v4l2_format *format)
{
    const uint32_t img_min_size = 40000U;
    const uint32_t img_compression_ratio = 6U;
    const uint32_t img_source_bpp = 16U;
    uint64_t pixels;
    uint64_t size;

    if (format == NULL || format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        return;
    }
    if (format->fmt.pix.sizeimage != 0) {
        return;
    }
    if (format->fmt.pix.pixelformat != V4L2_PIX_FMT_JPEG &&
            format->fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG &&
            format->fmt.pix.pixelformat != V4L2_PIX_FMT_H264) {
        return;
    }
    if (format->fmt.pix.width == 0 || format->fmt.pix.height == 0) {
        return;
    }

    pixels = (uint64_t)format->fmt.pix.width * format->fmt.pix.height;
    if (pixels == 0 || pixels > UINT32_MAX) {
        return;
    }

    /* esp_video consumes sizeimage in VIDIOC_S_FMT; estimate compressed buffers only when the driver did not provide one. */
    size = ((pixels * img_source_bpp / 8U) + img_compression_ratio - 1U) / img_compression_ratio;
    if (size < img_min_size) {
        size = img_min_size;
    }
    if (size > UINT32_MAX) {
        return;
    }

    format->fmt.pix.sizeimage = (uint32_t)size;
}

static esp_err_t camera_release_borrowed_slot_locked(camera_borrowed_frame_t *slot)
{
    esp_err_t ret = ESP_OK;

    if (slot == NULL || !slot->active) {
        return ESP_OK;
    }

    /* If a previous release failed, keep the mapping alive for outstanding Lua frames and finish closing after the last release. */
    if (!s_camera.close_pending && s_camera.fd >= 0) {
        if (camera_requeue_buffer_locked(&slot->buffer) != ESP_OK) {
            ESP_LOGE(TAG, "failed to requeue borrowed frame index=%u", (unsigned)slot->buffer.index);
            s_camera.close_pending = true;
            ret = ESP_FAIL;
        }
    }

    memset(slot, 0, sizeof(*slot));
    if (s_camera.borrowed_count > 0) {
        s_camera.borrowed_count--;
    }
    return ret;
}

static void camera_close_locked(void)
{
    if (s_camera.streaming && s_camera.fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(s_camera.fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF failed (errno=%d)", errno);
        }
    }

    for (uint32_t i = 0; i < s_camera.buffer_count; ++i) {
        if (s_camera.buffers[i] != NULL) {
            munmap(s_camera.buffers[i], s_camera.buffer_lengths[i]);
            s_camera.buffers[i] = NULL;
        }
        s_camera.buffer_lengths[i] = 0;
    }
    s_camera.buffer_count = 0;

    if (s_camera.fd >= 0) {
        struct v4l2_requestbuffers request = {
            .count = 0,
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        /* Release driver-owned buffers allocated by VIDIOC_REQBUFS. */
        if (ioctl(s_camera.fd, VIDIOC_REQBUFS, &request) != 0) {
            ESP_LOGW(TAG, "VIDIOC_REQBUFS release failed (errno=%d)", errno);
        }
    }

    if (s_camera.fd >= 0) {
        close(s_camera.fd);
        s_camera.fd = -1;
    }

    s_camera.opened = false;
    s_camera.streaming = false;
    s_camera.close_pending = false;
    s_camera.borrowed_count = 0;
    s_camera.settled_frames = 0;
    memset(s_camera.borrowed, 0, sizeof(s_camera.borrowed));
    s_camera.width = 0;
    s_camera.height = 0;
    s_camera.pixel_format = 0;
}

static void camera_close_or_defer_locked(const char *reason)
{
    if (s_camera.borrowed_count > 0) {
        ESP_LOGE(TAG, "%s; deferring camera close until %" PRIu32 " borrowed frame(s) are released", reason, s_camera.borrowed_count);
        s_camera.close_pending = true;
        return;
    }

    camera_close_locked();
}

static uint32_t camera_abs_diff_u32(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

static uint32_t camera_clamp_u32(uint32_t value, uint32_t min_value, uint32_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static uint32_t camera_round_to_step(uint32_t value, uint32_t min_value, uint32_t max_value, uint32_t step)
{
    uint32_t clamped = camera_clamp_u32(value, min_value, max_value);
    uint32_t lower;
    uint32_t upper;
    uint32_t offset;

    if (step == 0 || clamped <= min_value) {
        return clamped;
    }

    offset = clamped - min_value;
    lower = min_value + (offset / step) * step;
    upper = lower;
    if (upper <= UINT32_MAX - step) {
        upper += step;
    }
    if (upper > max_value) {
        upper = max_value;
    }
    return camera_abs_diff_u32(clamped, lower) <= camera_abs_diff_u32(clamped, upper) ? lower : upper;
}

static uint64_t camera_size_score(uint32_t width, uint32_t height, uint32_t target_width, uint32_t target_height)
{
    uint64_t dw = camera_abs_diff_u32(width, target_width);
    uint64_t dh = camera_abs_diff_u32(height, target_height);
    uint64_t area = (uint64_t)width * (uint64_t)height;
    uint64_t target_area = (uint64_t)target_width * (uint64_t)target_height;
    uint64_t da = area > target_area ? area - target_area : target_area - area;

    return (dw * dw) + (dh * dh) + (da / 4U);
}

static void camera_consider_size(uint32_t width, uint32_t height, uint32_t target_width, uint32_t target_height,
                                 bool *found, uint64_t *best_score, uint32_t *best_width, uint32_t *best_height)
{
    uint64_t score;

    if (width == 0 || height == 0) {
        return;
    }

    score = camera_size_score(width, height, target_width, target_height);
    if (!*found || score < *best_score) {
        *found = true;
        *best_score = score;
        *best_width = width;
        *best_height = height;
    }
}

static esp_err_t camera_find_closest_size_locked(uint32_t pixel_format, uint32_t target_width, uint32_t target_height,
                                                 uint32_t *out_width, uint32_t *out_height)
{
    bool found = false;
    uint64_t best_score = UINT64_MAX;
    uint32_t best_width = 0;
    uint32_t best_height = 0;

    if (target_width == 0 || target_height == 0 || out_width == NULL || out_height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t index = 0; ; index++) {
        struct v4l2_frmsizeenum fsize = {0};

        fsize.index = index;
        fsize.pixel_format = pixel_format;
        if (ioctl(s_camera.fd, VIDIOC_ENUM_FRAMESIZES, &fsize) != 0) {
            int saved_errno = errno;
            if (saved_errno == EINVAL || saved_errno == ENOTTY || saved_errno == ESRCH || saved_errno == ENODEV) {
                break;
            }
            ESP_LOGW(TAG, "VIDIOC_ENUM_FRAMESIZES(%u) failed during nearest-size search (errno=%d)", (unsigned)index, saved_errno);
            return ESP_FAIL;
        }

        switch (fsize.type) {
        case V4L2_FRMSIZE_TYPE_DISCRETE:
            camera_consider_size(fsize.discrete.width, fsize.discrete.height, target_width, target_height,
                                 &found, &best_score, &best_width, &best_height);
            break;
        case V4L2_FRMSIZE_TYPE_STEPWISE:
        case V4L2_FRMSIZE_TYPE_CONTINUOUS:
        {
            uint32_t step_width = fsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS ? 1 : fsize.stepwise.step_width;
            uint32_t step_height = fsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS ? 1 : fsize.stepwise.step_height;
            uint32_t width = camera_round_to_step(target_width, fsize.stepwise.min_width, fsize.stepwise.max_width, step_width);
            uint32_t height = camera_round_to_step(target_height, fsize.stepwise.min_height, fsize.stepwise.max_height, step_height);
            camera_consider_size(width, height, target_width, target_height, &found, &best_score, &best_width, &best_height);
            break;
        }
        default:
            ESP_LOGW(TAG, "VIDIOC_ENUM_FRAMESIZES returned unknown type=%u during nearest-size search", (unsigned)fsize.type);
            break;
        }
    }

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }

    *out_width = best_width;
    *out_height = best_height;
    return ESP_OK;
}

static esp_err_t camera_open_locked(const char *dev_path, const camera_open_opts_t *opts)
{
    struct v4l2_format format = {0};
    struct v4l2_requestbuffers request = {0};
    char pixel_format[5] = {0};
    uint32_t requested_width = 0;
    uint32_t requested_height = 0;
    uint32_t requested_pixel_format = 0;
    esp_err_t err;

    if (s_camera.opened) {
        if (s_camera.close_pending) {
            return ESP_ERR_INVALID_STATE;
        }
        /* Re-opening with explicit opts is ambiguous (resize? reformat?); make
         * the caller close first. Plain open(NULL opts) stays idempotent. */
        if (opts != NULL && (opts->width != 0 || opts->height != 0 || opts->pixel_format != 0)) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    if (dev_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_camera.fd = open(dev_path, O_RDWR);
    if (s_camera.fd < 0) {
        ESP_LOGE(TAG, "Failed to open %s (errno=%d)", dev_path, errno);
        s_camera.fd = -1;
        return ESP_ERR_NOT_FOUND;
    }

    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_camera.fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed (errno=%d)", errno);
        camera_close_locked();
        return ESP_FAIL;
    }

    requested_width = format.fmt.pix.width;
    requested_height = format.fmt.pix.height;
    requested_pixel_format = format.fmt.pix.pixelformat;
    if (opts != NULL && (opts->width != 0 || opts->height != 0 || opts->pixel_format != 0)) {
        requested_width = opts->width != 0 ? opts->width : format.fmt.pix.width;
        requested_height = opts->height != 0 ? opts->height : format.fmt.pix.height;
        requested_pixel_format = opts->pixel_format != 0 ? opts->pixel_format : format.fmt.pix.pixelformat;
        if (opts->width != 0) {
            format.fmt.pix.width = opts->width;
        }
        if (opts->height != 0) {
            format.fmt.pix.height = opts->height;
        }
        if (opts->pixel_format != 0) {
            format.fmt.pix.pixelformat = opts->pixel_format;
        }
    }

    camera_apply_internal_sizeimage(&format);
    if (ioctl(s_camera.fd, VIDIOC_S_FMT, &format) != 0) {
        int saved_errno = errno;
        ESP_LOGE(TAG, "VIDIOC_S_FMT failed for requested size=%ux%u format=0x%08" PRIx32 " sizeimage=%" PRIu32 " (errno=%d)",
                 (unsigned)requested_width, (unsigned)requested_height, requested_pixel_format, format.fmt.pix.sizeimage, saved_errno);
        camera_close_locked();
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Driver may have adjusted any field — adopt whatever it reports back. */

    s_camera.width = format.fmt.pix.width;
    s_camera.height = format.fmt.pix.height;
    s_camera.pixel_format = format.fmt.pix.pixelformat;
    camera_fourcc_to_string(s_camera.pixel_format, pixel_format);

    request.count = CAMERA_BUFFER_COUNT;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_camera.fd, VIDIOC_REQBUFS, &request) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed (errno=%d)", errno);
        camera_close_locked();
        return ESP_FAIL;
    }
    if (request.count == 0) {
        ESP_LOGE(TAG, "Camera buffer request returned 0");
        camera_close_locked();
        return ESP_ERR_NO_MEM;
    }

    s_camera.buffer_count = request.count > CAMERA_BUFFER_COUNT ?
                            CAMERA_BUFFER_COUNT : request.count;

    for (uint32_t i = 0; i < s_camera.buffer_count; ++i) {
        struct v4l2_buffer buffer = {0};

        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (ioctl(s_camera.fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF failed (errno=%d)", errno);
            camera_close_locked();
            return ESP_FAIL;
        }

        s_camera.buffers[i] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, s_camera.fd, buffer.m.offset);
        if (s_camera.buffers[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "Failed to mmap camera buffer (errno=%d)", errno);
            s_camera.buffers[i] = NULL;
            camera_close_locked();
            return ESP_ERR_NO_MEM;
        }
        s_camera.buffer_lengths[i] = buffer.length;

        if (ioctl(s_camera.fd, VIDIOC_QBUF, &buffer) != 0) {
            ESP_LOGE(TAG, "Initial VIDIOC_QBUF failed (errno=%d)", errno);
            camera_close_locked();
            return ESP_FAIL;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_camera.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed (errno=%d)", errno);
        camera_close_locked();
        return ESP_FAIL;
    }

    s_camera.streaming = true;
    s_camera.opened = true;
    s_camera.settled_frames = 0;

    ESP_LOGI(TAG, "Camera opened: path=%s size=%ux%u format=%s sizeimage=%" PRIu32 " buffers=%" PRIu32 " buffer_len=%zu",
             dev_path, (unsigned)s_camera.width, (unsigned)s_camera.height,
             pixel_format, format.fmt.pix.sizeimage, s_camera.buffer_count,
             s_camera.buffer_count > 0 ? s_camera.buffer_lengths[0] : 0);

    /* Settle the stream at open time with a dedicated timeout so that capture()
     * calls are not penalised by the sensor warm-up delay. */
    int64_t settle_deadline_us =
        esp_timer_get_time() + ((int64_t)CAMERA_SETTLE_TIMEOUT_MS * 1000LL);
    err = camera_settle_stream_locked(settle_deadline_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stream settle failed during open (err=0x%x)", err);
        camera_close_locked();
        return err;
    }

    return ESP_OK;
}

static esp_err_t camera_dequeue_buffer_locked(int timeout_ms, struct v4l2_buffer *buffer)
{
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    if (buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* esp_video VFS does not implement poll(); use VIDIOC_S_DQBUF_TIMEOUT to
     * set a per-call timeout and block directly in VIDIOC_DQBUF. */
    if (ioctl(s_camera.fd, VIDIOC_S_DQBUF_TIMEOUT, &tv) != 0) {
        ESP_LOGW(TAG, "VIDIOC_S_DQBUF_TIMEOUT failed (errno=%d)", errno);
        return ESP_FAIL;
    }

    memset(buffer, 0, sizeof(*buffer));
    buffer->type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer->memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_camera.fd, VIDIOC_DQBUF, buffer) != 0) {
        int saved_errno = errno;
        if (saved_errno == ETIMEDOUT) {
            ESP_LOGW(TAG, "VIDIOC_DQBUF timeout after %d ms", timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        /* esp_video collapses both "queue empty" and "real error" into
         * ESP_FAIL → errno=EPERM, so warning on EPERM only adds noise. Other
         * errnos still warn since they indicate something more specific. */
        if (saved_errno != EPERM) {
            ESP_LOGW(TAG, "VIDIOC_DQBUF failed (errno=%d)", saved_errno);
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t camera_requeue_buffer_locked(struct v4l2_buffer *buffer)
{
    if (buffer == NULL || buffer->index >= s_camera.buffer_count) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ioctl(s_camera.fd, VIDIOC_QBUF, buffer) != 0) {
        ESP_LOGW(TAG, "VIDIOC_QBUF failed (errno=%d)", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t camera_validate_buffer_locked(const struct v4l2_buffer *buffer,
                                               size_t *frame_bytes)
{
    if (buffer == NULL || frame_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((buffer->flags & V4L2_BUF_FLAG_DONE) == 0 || (buffer->flags & V4L2_BUF_FLAG_ERROR) != 0) {
        return ESP_FAIL;
    }
    if (buffer->index >= s_camera.buffer_count) {
        return ESP_FAIL;
    }
    if (s_camera.buffers[buffer->index] == NULL || s_camera.buffer_lengths[buffer->index] == 0) {
        return ESP_FAIL;
    }
    if (buffer->bytesused > s_camera.buffer_lengths[buffer->index]) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (buffer->bytesused == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    *frame_bytes = buffer->bytesused;
    return ESP_OK;
}

static esp_err_t camera_settle_stream_locked(int64_t deadline_us)
{
    while (s_camera.settled_frames < CAMERA_STREAM_SETTLE_FRAMES) {
        struct v4l2_buffer buffer = {0};
        size_t frame_bytes = 0;
        int remaining_ms = camera_remaining_timeout_ms(deadline_us);
        esp_err_t err;

        if (remaining_ms <= 0) {
            ESP_LOGW(TAG, "settle: timeout (settled=%" PRIu32 "/%d)",
                     s_camera.settled_frames, CAMERA_STREAM_SETTLE_FRAMES);
            return ESP_ERR_TIMEOUT;
        }

        err = camera_dequeue_buffer_locked(remaining_ms, &buffer);
        if (err != ESP_OK) {
            camera_close_or_defer_locked("settle dequeue failed");
            return err;
        }

        err = camera_validate_buffer_locked(&buffer, &frame_bytes);
        if (camera_requeue_buffer_locked(&buffer) != ESP_OK) {
            camera_close_or_defer_locked("settle requeue failed");
            return ESP_FAIL;
        }

        if (err == ESP_OK && frame_bytes > 0) {
            s_camera.settled_frames++;
        }
    }
    return ESP_OK;
}

static esp_err_t camera_get_frame_locked(int timeout_ms, struct v4l2_buffer *buffer,
                                         uint8_t **frame_data, size_t *frame_bytes,
                                         int64_t *timestamp_us)
{
    int64_t deadline_us;
    esp_err_t err;

    if (buffer == NULL || frame_data == NULL || frame_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_camera.opened || s_camera.close_pending) {
        return ESP_ERR_INVALID_STATE;
    }
    deadline_us = esp_timer_get_time() + ((int64_t)camera_effective_timeout_ms(timeout_ms) * 1000LL);

    err = camera_settle_stream_locked(deadline_us);
    if (err != ESP_OK) {
        return err;
    }

    while (camera_remaining_timeout_ms(deadline_us) > 0) {
        int remaining_ms = camera_remaining_timeout_ms(deadline_us);

        err = camera_dequeue_buffer_locked(remaining_ms, buffer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "camera dequeue failed while capturing: %s", esp_err_to_name(err));
            return err;
        }

        err = camera_validate_buffer_locked(buffer, frame_bytes);
        if (err == ESP_OK) {
            *frame_data = s_camera.buffers[buffer->index];
            if (timestamp_us != NULL) {
                *timestamp_us = esp_timer_get_time();
            }
            return ESP_OK;
        }

        if (camera_requeue_buffer_locked(buffer) != ESP_OK) {
            camera_close_or_defer_locked("capture requeue failed");
            return ESP_FAIL;
        }
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t camera_open(const char *dev_path, const camera_open_opts_t *opts)
{
    esp_err_t err = camera_lock();

    if (err != ESP_OK) {
        return err;
    }

    err = camera_open_locked(dev_path, opts);
    camera_unlock();
    return err;
}

esp_err_t camera_get_stream_info(camera_stream_info_t *out_info)
{
    esp_err_t err;

    if (out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_info, 0, sizeof(*out_info));

    err = camera_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_camera.opened) {
        camera_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    out_info->width = s_camera.width;
    out_info->height = s_camera.height;
    out_info->pixel_format = s_camera.pixel_format;
    camera_fourcc_to_string(s_camera.pixel_format, out_info->pixel_format_str);

    camera_unlock();
    return ESP_OK;
}

esp_err_t camera_capture_frame(int timeout_ms, uint8_t **frame_data_out, size_t *frame_bytes_out,
                               camera_frame_info_t *out_info)
{
    struct v4l2_buffer buffer = {0};
    uint8_t *frame_data = NULL;
    size_t frame_bytes = 0;
    int64_t timestamp_us = 0;
    esp_err_t err;

    if (frame_data_out == NULL || frame_bytes_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *frame_data_out = NULL;
    *frame_bytes_out = 0;
    if (out_info != NULL) {
        memset(out_info, 0, sizeof(*out_info));
    }

    err = camera_lock();
    if (err != ESP_OK) {
        return err;
    }

    err = camera_get_frame_locked(timeout_ms, &buffer, &frame_data, &frame_bytes, &timestamp_us);
    if (err != ESP_OK) {
        camera_unlock();
        return err;
    }

    if (buffer.index >= CAMERA_BUFFER_COUNT || buffer.index >= s_camera.buffer_count) {
        ESP_LOGE(TAG, "captured buffer index out of range: %u", (unsigned)buffer.index);
        if (camera_requeue_buffer_locked(&buffer) != ESP_OK) {
            camera_close_or_defer_locked("capture invalid-index requeue failed");
        }
        camera_unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_camera.borrowed[buffer.index].active) {
        ESP_LOGE(TAG, "captured buffer index already borrowed: %u", (unsigned)buffer.index);
        if (camera_requeue_buffer_locked(&buffer) != ESP_OK) {
            camera_close_or_defer_locked("capture duplicate-index requeue failed");
        }
        camera_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_camera.borrowed[buffer.index].active = true;
    s_camera.borrowed[buffer.index].buffer = buffer;
    s_camera.borrowed[buffer.index].ptr = frame_data;
    s_camera.borrowed[buffer.index].bytes = frame_bytes;
    s_camera.borrowed_count++;

    *frame_data_out = frame_data;
    *frame_bytes_out = frame_bytes;
    if (out_info != NULL) {
        out_info->width = s_camera.width;
        out_info->height = s_camera.height;
        out_info->pixel_format = s_camera.pixel_format;
        camera_fourcc_to_string(s_camera.pixel_format, out_info->pixel_format_str);
        out_info->frame_bytes = frame_bytes;
        out_info->timestamp_us = timestamp_us;
    }

    camera_unlock();
    return ESP_OK;
}

esp_err_t camera_release_frame(void *frame_data)
{
    camera_borrowed_frame_t *slot = NULL;
    esp_err_t err = camera_lock();

    if (err != ESP_OK) {
        return err;
    }

    if (s_camera.borrowed_count == 0) {
        camera_unlock();
        return ESP_OK;
    }
    if (frame_data == NULL) {
        ESP_LOGE(TAG, "release_frame requires a frame pointer when multiple frames may be borrowed");
        camera_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = 0; i < s_camera.buffer_count; i++) {
        if (s_camera.borrowed[i].active && s_camera.borrowed[i].ptr == frame_data) {
            slot = &s_camera.borrowed[i];
            break;
        }
    }
    if (slot == NULL) {
        ESP_LOGE(TAG, "release_frame got unknown frame pointer");
        camera_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    err = camera_release_borrowed_slot_locked(slot);
    if (s_camera.close_pending && s_camera.borrowed_count == 0) {
        camera_close_locked();
    }

    camera_unlock();
    return err;
}

esp_err_t camera_close(void)
{
    esp_err_t err = camera_lock();

    if (err != ESP_OK) {
        return err;
    }

    if (s_camera.borrowed_count > 0) {
        ESP_LOGE(TAG, "close rejected: %" PRIu32 " frame(s) still borrowed", s_camera.borrowed_count);
        camera_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    camera_close_locked();
    camera_unlock();
    return ESP_OK;
}

esp_err_t camera_flush(void)
{
    esp_err_t err = camera_lock();
    int drained = 0;

    if (err != ESP_OK) {
        return err;
    }

    if (!s_camera.opened || !s_camera.streaming || s_camera.close_pending) {
        camera_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_camera.borrowed_count > 0) {
        ESP_LOGW(TAG, "flush rejected: %" PRIu32 " frame(s) still borrowed", s_camera.borrowed_count);
        camera_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    /* Drain every ready buffer. Any non-OK dequeue means "no buffer available
     * right now" and we treat it as the empty-queue signal — esp_video does
     * not distinguish empty-queue from hard error on VIDIOC_DQBUF (both come
     * back as ESP_FAIL → errno=EPERM in its VFS), so we cannot be more
     * selective here without false negatives. */
    while (true) {
        struct v4l2_buffer buffer = {0};
        esp_err_t dq = camera_dequeue_buffer_locked(1 /* ms */, &buffer);
        if (dq != ESP_OK) {
            break;
        }
        if (camera_requeue_buffer_locked(&buffer) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        drained++;
    }

    /* Force a re-settle on the next get_frame so AE/AWB-stale buffers aren't
     * returned right after a long pause. */
    s_camera.settled_frames = 0;
    camera_unlock();

    if (err == ESP_OK) {
        ESP_LOGD(TAG, "flush drained %d buffer(s)", drained);
    }
    return err;
}

bool camera_is_open(void)
{
    bool opened;

    if (camera_lock() != ESP_OK) {
        return false;
    }
    opened = s_camera.opened;
    camera_unlock();
    return opened;
}

bool camera_is_streaming(void)
{
    bool streaming;

    if (camera_lock() != ESP_OK) {
        return false;
    }
    streaming = s_camera.opened && s_camera.streaming && !s_camera.close_pending;
    camera_unlock();
    return streaming;
}

esp_err_t camera_get_borrowed_count(uint32_t *out_count)
{
    esp_err_t err;

    if (out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = camera_lock();
    if (err != ESP_OK) {
        return err;
    }

    *out_count = s_camera.borrowed_count;
    camera_unlock();
    return ESP_OK;
}

esp_err_t camera_enum_format(uint32_t index, camera_format_desc_t *out)
{
    struct v4l2_fmtdesc desc = {0};
    esp_err_t err;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    err = camera_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_camera.opened) {
        camera_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    desc.index = index;
    if (ioctl(s_camera.fd, VIDIOC_ENUM_FMT, &desc) != 0) {
        int saved_errno = errno;
        camera_unlock();
        /* EINVAL marks "past the end" for V4L2 enumeration ioctls. */
        if (saved_errno == EINVAL) {
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGW(TAG, "VIDIOC_ENUM_FMT(%u) failed (errno=%d)", (unsigned)index, saved_errno);
        return ESP_FAIL;
    }

    out->pixel_format = desc.pixelformat;
    camera_fourcc_to_string(desc.pixelformat, out->pixel_format_str);
    strlcpy(out->description, (const char *)desc.description, sizeof(out->description));
    out->flags = desc.flags;
    out->compressed = (desc.flags & V4L2_FMT_FLAG_COMPRESSED) != 0;
    out->emulated = (desc.flags & V4L2_FMT_FLAG_EMULATED) != 0;
    camera_unlock();
    return ESP_OK;
}

esp_err_t camera_enum_frame_size(uint32_t pixel_format, uint32_t index,
                                 camera_frame_size_t *out)
{
    struct v4l2_frmsizeenum fsize = {0};
    esp_err_t err;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    err = camera_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_camera.opened) {
        camera_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    fsize.index = index;
    fsize.pixel_format = pixel_format;
    if (ioctl(s_camera.fd, VIDIOC_ENUM_FRAMESIZES, &fsize) != 0) {
        int saved_errno = errno;
        camera_unlock();
        /* EINVAL/ENOTTY = normal "end of enumeration" reply.
         * ESRCH = esp_video maps ESP_ERR_NOT_SUPPORTED here (device has no
         *         enum_framesizes op, or op rejects this pixel format —
         *         e.g. CSI bypass mode with a JPEG sensor).
         * ENODEV = esp_video maps ESP_ERR_NOT_FOUND here (UVC device disconnected,
         *          or other "device gone" condition).
         * For all of these we fall back to G_FMT instead of failing the script. */
        if (saved_errno == EINVAL || saved_errno == ENOTTY ||
            saved_errno == ESRCH || saved_errno == ENODEV) {
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGW(TAG, "VIDIOC_ENUM_FRAMESIZES(%u) failed (errno=%d)", (unsigned)index, saved_errno);
        return ESP_FAIL;
    }

    switch (fsize.type) {
    case V4L2_FRMSIZE_TYPE_DISCRETE:
        out->type = CAMERA_FRAME_SIZE_DISCRETE;
        out->width = fsize.discrete.width;
        out->height = fsize.discrete.height;
        break;
    case V4L2_FRMSIZE_TYPE_STEPWISE:
        out->type = CAMERA_FRAME_SIZE_STEPWISE;
        out->min_width = fsize.stepwise.min_width;
        out->max_width = fsize.stepwise.max_width;
        out->step_width = fsize.stepwise.step_width;
        out->min_height = fsize.stepwise.min_height;
        out->max_height = fsize.stepwise.max_height;
        out->step_height = fsize.stepwise.step_height;
        break;
    case V4L2_FRMSIZE_TYPE_CONTINUOUS:
        out->type = CAMERA_FRAME_SIZE_CONTINUOUS;
        out->min_width = fsize.stepwise.min_width;
        out->max_width = fsize.stepwise.max_width;
        out->step_width = 1;
        out->min_height = fsize.stepwise.min_height;
        out->max_height = fsize.stepwise.max_height;
        out->step_height = 1;
        break;
    default:
        camera_unlock();
        ESP_LOGW(TAG, "VIDIOC_ENUM_FRAMESIZES returned unknown type=%u", (unsigned)fsize.type);
        return ESP_FAIL;
    }

    camera_unlock();
    return ESP_OK;
}

esp_err_t camera_find_closest_size(uint32_t pixel_format,
                                   uint32_t target_width, uint32_t target_height,
                                   uint32_t *out_width, uint32_t *out_height)
{
    esp_err_t err;

    if (target_width == 0 || target_height == 0 || out_width == NULL || out_height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = camera_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_camera.opened) {
        camera_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    err = camera_find_closest_size_locked(pixel_format, target_width, target_height, out_width, out_height);
    camera_unlock();
    return err;
}

esp_err_t camera_enum_frame_interval(uint32_t pixel_format,
                                     uint32_t width, uint32_t height,
                                     uint32_t index,
                                     camera_frame_interval_t *out)
{
    struct v4l2_frmivalenum fival = {0};
    esp_err_t err;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    err = camera_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_camera.opened) {
        camera_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    fival.index = index;
    fival.pixel_format = pixel_format;
    fival.width = width;
    fival.height = height;
    if (ioctl(s_camera.fd, VIDIOC_ENUM_FRAMEINTERVALS, &fival) != 0) {
        int saved_errno = errno;
        camera_unlock();
        if (saved_errno == EINVAL || saved_errno == ENOTTY ||
            saved_errno == ESRCH || saved_errno == ENODEV) {
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGW(TAG, "VIDIOC_ENUM_FRAMEINTERVALS(%u) failed (errno=%d)", (unsigned)index, saved_errno);
        return ESP_FAIL;
    }

    switch (fival.type) {
    case V4L2_FRMIVAL_TYPE_DISCRETE:
        out->type = CAMERA_FRAME_SIZE_DISCRETE;
        out->numerator = fival.discrete.numerator;
        out->denominator = fival.discrete.denominator;
        break;
    case V4L2_FRMIVAL_TYPE_STEPWISE:
        out->type = CAMERA_FRAME_SIZE_STEPWISE;
        out->min_numerator = fival.stepwise.min.numerator;
        out->min_denominator = fival.stepwise.min.denominator;
        out->max_numerator = fival.stepwise.max.numerator;
        out->max_denominator = fival.stepwise.max.denominator;
        out->step_numerator = fival.stepwise.step.numerator;
        out->step_denominator = fival.stepwise.step.denominator;
        break;
    case V4L2_FRMIVAL_TYPE_CONTINUOUS:
        out->type = CAMERA_FRAME_SIZE_CONTINUOUS;
        out->min_numerator = fival.stepwise.min.numerator;
        out->min_denominator = fival.stepwise.min.denominator;
        out->max_numerator = fival.stepwise.max.numerator;
        out->max_denominator = fival.stepwise.max.denominator;
        out->step_numerator = 1;
        out->step_denominator = 1;
        break;
    default:
        camera_unlock();
        ESP_LOGW(TAG, "VIDIOC_ENUM_FRAMEINTERVALS returned unknown type=%u", (unsigned)fival.type);
        return ESP_FAIL;
    }

    camera_unlock();
    return ESP_OK;
}
