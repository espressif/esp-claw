/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_private.h"

void audio_converter_destroy(audio_converter_t *converter)
{
    if (!converter) {
        return;
    }
    if (converter->handle) {
        esp_asrc_close(converter->handle);
    }
    free(converter->in_buf);
    free(converter->out_buf);
    memset(converter, 0, sizeof(*converter));
}

esp_err_t audio_converter_create(audio_converter_t *converter, const audio_format_t *src, const audio_format_t *dst)
{
    esp_asrc_err_t err;
    char src_buf[32];
    char dst_buf[32];

    memset(converter, 0, sizeof(*converter));
    converter->src = *src;
    converter->dst = *dst;
    converter->in_frame_bytes = src->bytes_per_frame;
    converter->out_frame_bytes = dst->bytes_per_frame;

    if (audio_format_equal(src, dst)) {
        converter->bypass = true;
        return ESP_OK;
    }

    esp_asrc_cfg_t cfg = {
        .src_info = {
            .sample_rate = src->sample_rate,
            .channel = src->channels,
            .bits_per_sample = src->bits,
        },
        .dest_info = {
            .sample_rate = dst->sample_rate,
            .channel = dst->channels,
            .bits_per_sample = dst->bits,
        },
        .weight = NULL,
        .weight_len = 0,
        .perf_type = ESP_ASRC_PERF_TYPE_AUTO,
        .complexity = AUDIO_ASRC_COMPLEXITY,
        .timeout_ms = AUDIO_ASRC_TIMEOUT_MS,
    };

    err = esp_asrc_open(&cfg, &converter->handle);
    if (err != ESP_ASRC_ERR_OK) {
        audio_format_log(src_buf, sizeof(src_buf), src);
        audio_format_log(dst_buf, sizeof(dst_buf), dst);
        ESP_LOGE(TAG, "ASRC open failed: %s -> %s, err=%d", src_buf, dst_buf, err);
        return ESP_FAIL;
    }

    err = esp_asrc_get_buffer_alignment(&converter->align);
    if (err != ESP_ASRC_ERR_OK) {
        ESP_LOGE(TAG, "ASRC get alignment failed: err=%d", err);
        audio_converter_destroy(converter);
        return ESP_FAIL;
    }
    if (esp_asrc_get_bytes_per_sample(converter->handle, &converter->in_frame_bytes, &converter->out_frame_bytes) != ESP_ASRC_ERR_OK) {
        ESP_LOGE(TAG, "ASRC get frame bytes failed");
        audio_converter_destroy(converter);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t audio_converter_ensure_buffers(audio_converter_t *converter, uint32_t in_bytes, uint32_t out_bytes)
{
    if (converter->in_buf_size < in_bytes) {
        uint32_t allocated = 0;
        uint8_t *new_buf = (uint8_t *)esp_asrc_align_alloc(in_bytes, converter->align.inbuf_addr_align, converter->align.inbuf_size_align, &allocated);
        if (!new_buf) {
            ESP_LOGE(TAG, "ASRC input buffer alloc failed: %" PRIu32 " bytes", in_bytes);
            return ESP_ERR_NO_MEM;
        }
        free(converter->in_buf);
        converter->in_buf = new_buf;
        converter->in_buf_size = allocated;
    }
    if (converter->out_buf_size < out_bytes) {
        uint32_t allocated = 0;
        uint8_t *new_buf = (uint8_t *)esp_asrc_align_alloc(out_bytes, converter->align.outbuf_addr_align, converter->align.outbuf_size_align, &allocated);
        if (!new_buf) {
            ESP_LOGE(TAG, "ASRC output buffer alloc failed: %" PRIu32 " bytes", out_bytes);
            return ESP_ERR_NO_MEM;
        }
        free(converter->out_buf);
        converter->out_buf = new_buf;
        converter->out_buf_size = allocated;
    }
    return ESP_OK;
}

esp_err_t audio_converter_process(audio_converter_t *converter, const uint8_t *in, uint32_t in_bytes, uint8_t **out, uint32_t *out_bytes)
{
    uint32_t in_frames;
    uint32_t out_frames = 0;
    uint32_t max_out_frames;
    esp_asrc_err_t err;

    if (!converter || !in || !out || !out_bytes || in_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (converter->bypass) {
        *out = (uint8_t *)in;
        *out_bytes = in_bytes;
        return ESP_OK;
    }
    if ((in_bytes % converter->in_frame_bytes) != 0) {
        ESP_LOGE(TAG, "ASRC input not frame-aligned: bytes=%" PRIu32 ", frame=%u", in_bytes, converter->in_frame_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    in_frames = in_bytes / converter->in_frame_bytes;
    err = esp_asrc_get_out_sample_num(converter->handle, in_frames, &out_frames);
    if (err != ESP_ASRC_ERR_OK || out_frames == 0) {
        ESP_LOGE(TAG, "ASRC output frame estimate failed: err=%d", err);
        return ESP_FAIL;
    }
    if (audio_converter_ensure_buffers(converter, in_bytes, out_frames * converter->out_frame_bytes) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(converter->in_buf, in, in_bytes);
    max_out_frames = converter->out_buf_size / converter->out_frame_bytes;
    err = esp_asrc_process(converter->handle, converter->in_buf, in_frames, converter->out_buf, &max_out_frames);
    if (err != ESP_ASRC_ERR_OK) {
        char src_buf[32];
        char dst_buf[32];
        audio_format_log(src_buf, sizeof(src_buf), &converter->src);
        audio_format_log(dst_buf, sizeof(dst_buf), &converter->dst);
        ESP_LOGE(TAG, "ASRC process failed: %s -> %s, err=%d", src_buf, dst_buf, err);
        return ESP_FAIL;
    }

    *out = converter->out_buf;
    *out_bytes = max_out_frames * converter->out_frame_bytes;
    return ESP_OK;
}
