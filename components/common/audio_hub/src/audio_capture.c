/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* audio_capture — see include/audio_capture.h. */

#include "audio_capture.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "esp_ae_bit_cvt.h"
#include "esp_ae_ch_cvt.h"
#include "esp_ae_rate_cvt.h"
#include "esp_board_manager.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_log.h"

#include "claw_hw_registry.h"
#include "dev_audio_codec.h"
#include "audio_memory.h"

static const char *TAG = "audio_capture";

#define AUDIO_CAPTURE_TASK_PRIO         10
#define AUDIO_CAPTURE_TASK_STACK        4096
#define AUDIO_CAPTURE_OWNER_TAG         "capture/system"
#define AUDIO_CAPTURE_SUB_ROLE_COUNT    2
#define AUDIO_CAPTURE_IDLE_DELAY_MS     20
#define AUDIO_CAPTURE_IO_SLICE_MS       20
#define AUDIO_CAPTURE_CODEC_DEVICE_NAME "audio_adc"
/* Preserve the existing app_claw capture level; callers can lower it at runtime. */
#define AUDIO_CAPTURE_DEFAULT_INPUT_GAIN_DB AUDIO_CAPTURE_INPUT_GAIN_DB_MAX

/* UAC virtual registers are optional and identify themselves by magic. */
#define AUDIO_CAPTURE_UAC_FORMAT_MAGIC_REG       0x7AC0
#define AUDIO_CAPTURE_UAC_FORMAT_SAMPLE_RATE_REG 0x7AC1
#define AUDIO_CAPTURE_UAC_FORMAT_CHANNELS_REG    0x7AC2
#define AUDIO_CAPTURE_UAC_FORMAT_BITS_REG        0x7AC3
#define AUDIO_CAPTURE_UAC_FORMAT_MAGIC_VALUE     0x55414346

struct audio_capture_subscriber {
    audio_capture_subscriber_role_t role;
    audio_capture_sub_format_t      fmt;
    bool                            open;

    esp_ae_rate_cvt_handle_t        rate_cvt;
    esp_ae_ch_cvt_handle_t          ch_cvt;
    esp_ae_bit_cvt_handle_t         bit_cvt;

    uint8_t *rate_buf; size_t rate_buf_size;
    uint8_t *ch_buf;   size_t ch_buf_size;
    uint8_t *bit_buf;  size_t bit_buf_size;
    uint32_t rate_max_samples;

    StreamBufferHandle_t     ring;
    SemaphoreHandle_t        io_mtx;
    uint64_t                 dropped_bytes;
    char                    *owner_tag;
    struct audio_capture_t  *parent;
};

/* StreamBuffer requires one writer and one reader. Drop new data when a
 * subscriber falls behind, preserving the existing queued audio. */
static size_t capture_ring_send_drop_new(struct audio_capture_subscriber *sub, const void *data, size_t bytes)
{
    StreamBufferHandle_t sb = sub ? sub->ring : NULL;
    if (sb == NULL || data == NULL || bytes == 0) {
        return 0;
    }
    size_t dropped = 0;
    size_t sent = xStreamBufferSend(sb, data, bytes, 0);
    dropped += bytes - sent;
    if (dropped > 0) {
        sub->dropped_bytes += dropped;
        ESP_LOGW(TAG, "sub ring overflow role=%d owner=%s drop=%u total=%" PRIu64, (int)sub->role, sub->owner_tag ? sub->owner_tag : "?", (unsigned)dropped, sub->dropped_bytes);
    }
    return sent;
}

struct audio_capture_t {
    audio_capture_config_t     cfg;
    dev_audio_codec_handles_t *codec_handles;
    esp_codec_dev_handle_t     codec_dev;
    claw_hw_lease_handle_t     dev_lease;
    char                       device_key[48];

    size_t                     bytes_per_frame;
    uint32_t                   samples_per_frame;
    size_t                     frame_bytes;

    uint8_t                   *hub_frame;

    struct audio_capture_subscriber subs[AUDIO_CAPTURE_SUB_ROLE_COUNT];

    SemaphoreHandle_t          mtx;
    SemaphoreHandle_t          task_done;
    TaskHandle_t               task;
    volatile bool              running;
    bool                       codec_open;
    float                      input_gain_db;
};

static void capture_apply_defaults(audio_capture_config_t *cfg)
{
    if (cfg->sample_rate == 0) cfg->sample_rate = 16000;
    if (cfg->channels == 0)    cfg->channels = 1;
    if (cfg->bits == 0)        cfg->bits = 16;
    if (cfg->frame_ms == 0)    cfg->frame_ms = 20;
    if (cfg->ring_frames == 0) cfg->ring_frames = 8;
}

static esp_err_t capture_apply_uac_format(struct audio_capture_t *h)
{
    int magic = 0;
    int ret = esp_codec_dev_read_reg(h->codec_dev, AUDIO_CAPTURE_UAC_FORMAT_MAGIC_REG, &magic);
    if (ret != ESP_CODEC_DEV_OK || magic != AUDIO_CAPTURE_UAC_FORMAT_MAGIC_VALUE) {
        return ESP_OK;
    }

    int sample_rate = 0;
    int channels = 0;
    int bits = 0;
    ret = esp_codec_dev_read_reg(h->codec_dev, AUDIO_CAPTURE_UAC_FORMAT_SAMPLE_RATE_REG, &sample_rate);
    if (ret == ESP_CODEC_DEV_OK) ret = esp_codec_dev_read_reg(h->codec_dev, AUDIO_CAPTURE_UAC_FORMAT_CHANNELS_REG, &channels);
    if (ret == ESP_CODEC_DEV_OK) ret = esp_codec_dev_read_reg(h->codec_dev, AUDIO_CAPTURE_UAC_FORMAT_BITS_REG, &bits);
    if (ret != ESP_CODEC_DEV_OK || sample_rate <= 0 || channels <= 0 || bits <= 0 || bits % 8 != 0) {
        ESP_LOGE(TAG, "invalid UAC format ret=%d rate=%d ch=%d bits=%d", ret, sample_rate, channels, bits);
        return ESP_ERR_INVALID_STATE;
    }

    h->cfg.sample_rate = (uint32_t)sample_rate;
    h->cfg.channels = (uint8_t)channels;
    h->cfg.bits = (uint8_t)bits;
    ESP_LOGI(TAG, "using UAC format rate=%d ch=%d bits=%d", sample_rate, channels, bits);
    return ESP_OK;
}

static void capture_delete_ring(struct audio_capture_subscriber *sub)
{
    if (!sub || !sub->ring) {
        return;
    }
    audio_mem_stream_buffer_delete(sub->ring);
    sub->ring = NULL;
}

static bool capture_gain_is_valid(float gain_db)
{
    return gain_db >= 0.0f && gain_db <= AUDIO_CAPTURE_INPUT_GAIN_DB_MAX;
}

static bool fmt_matches_hub(const audio_capture_sub_format_t *fmt, const audio_capture_config_t *hub)
{
    return fmt->sample_rate == hub->sample_rate &&
           fmt->channels    == hub->channels &&
           fmt->bits        == hub->bits;
}

static void sub_reset_conv(struct audio_capture_subscriber *s)
{
    if (s->rate_cvt) { esp_ae_rate_cvt_close(s->rate_cvt); s->rate_cvt = NULL; }
    if (s->ch_cvt)   { esp_ae_ch_cvt_close(s->ch_cvt);     s->ch_cvt = NULL; }
    if (s->bit_cvt)  { esp_ae_bit_cvt_close(s->bit_cvt);   s->bit_cvt = NULL; }
    if (s->rate_buf) { audio_mem_free(s->rate_buf); s->rate_buf = NULL; s->rate_buf_size = 0; }
    if (s->ch_buf)   { audio_mem_free(s->ch_buf);   s->ch_buf = NULL;   s->ch_buf_size = 0; }
    if (s->bit_buf)  { audio_mem_free(s->bit_buf);  s->bit_buf = NULL;  s->bit_buf_size = 0; }
    s->rate_max_samples = 0;
}

/* Pipeline: rate -> channels -> bits. Any stage that already matches the
 * hub format is skipped. */
static esp_err_t sub_build_conv(struct audio_capture_subscriber *s, const struct audio_capture_t *hub)
{
    const audio_capture_config_t *hub_cfg = &hub->cfg;
    audio_capture_sub_format_t   *tgt = &s->fmt;

    uint32_t stage_rate = hub_cfg->sample_rate;
    uint8_t  stage_ch   = hub_cfg->channels;
    uint8_t  stage_bits = hub_cfg->bits;

    if (tgt->sample_rate != 0 && tgt->sample_rate != stage_rate) {
        esp_ae_rate_cvt_cfg_t cfg = {
            .src_rate        = stage_rate,
            .dest_rate       = tgt->sample_rate,
            .channel         = stage_ch,
            .bits_per_sample = stage_bits,
            .complexity      = 2,
            .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY,
        };
        esp_ae_err_t aerr = esp_ae_rate_cvt_open(&cfg, &s->rate_cvt);
        if (aerr != ESP_AE_ERR_OK || s->rate_cvt == NULL) {
            ESP_LOGE(TAG, "rate_cvt_open failed: %d", (int)aerr);
            return ESP_ERR_INVALID_STATE;
        }
        uint32_t max_out = 0;
        aerr = esp_ae_rate_cvt_get_max_out_sample_num(s->rate_cvt, hub->samples_per_frame, &max_out);
        if (aerr != ESP_AE_ERR_OK || max_out == 0) {
            ESP_LOGE(TAG, "rate_cvt_get_max_out failed: %d", (int)aerr);
            return ESP_ERR_INVALID_STATE;
        }
        s->rate_max_samples = max_out;
        s->rate_buf_size = (size_t)max_out * stage_ch * (stage_bits / 8);
        s->rate_buf = audio_mem_aligned_alloc(16, s->rate_buf_size);
        if (s->rate_buf == NULL) return ESP_ERR_NO_MEM;
        stage_rate = tgt->sample_rate;
    } else {
        s->rate_max_samples = hub->samples_per_frame;
    }

    if (tgt->channels != 0 && tgt->channels != stage_ch) {
        esp_ae_ch_cvt_cfg_t cfg = {
            .sample_rate     = stage_rate,
            .bits_per_sample = stage_bits,
            .src_ch          = stage_ch,
            .dest_ch         = tgt->channels,
            .weight          = NULL,
            .weight_len      = 0,
        };
        esp_ae_err_t aerr = esp_ae_ch_cvt_open(&cfg, &s->ch_cvt);
        if (aerr != ESP_AE_ERR_OK || s->ch_cvt == NULL) {
            ESP_LOGE(TAG, "ch_cvt_open failed: %d", (int)aerr);
            return ESP_ERR_INVALID_STATE;
        }
        s->ch_buf_size = (size_t)s->rate_max_samples * tgt->channels * (stage_bits / 8);
        s->ch_buf = audio_mem_aligned_alloc(16, s->ch_buf_size);
        if (s->ch_buf == NULL) return ESP_ERR_NO_MEM;
        stage_ch = tgt->channels;
    }

    if (tgt->bits != 0 && tgt->bits != stage_bits) {
        esp_ae_bit_cvt_cfg_t cfg = {
            .sample_rate     = stage_rate,
            .channel         = stage_ch,
            .src_bits        = stage_bits,
            .dest_bits       = tgt->bits,
        };
        esp_ae_err_t aerr = esp_ae_bit_cvt_open(&cfg, &s->bit_cvt);
        if (aerr != ESP_AE_ERR_OK || s->bit_cvt == NULL) {
            ESP_LOGE(TAG, "bit_cvt_open failed: %d", (int)aerr);
            return ESP_ERR_INVALID_STATE;
        }
        s->bit_buf_size = (size_t)s->rate_max_samples * stage_ch * (tgt->bits / 8);
        s->bit_buf = audio_mem_aligned_alloc(16, s->bit_buf_size);
        if (s->bit_buf == NULL) return ESP_ERR_NO_MEM;
        stage_bits = tgt->bits;
    }

    /* Persist the resolved target so sub_info reports the actual format
     * even when the caller left some fields at 0. */
    tgt->sample_rate = stage_rate;
    tgt->channels    = stage_ch;
    tgt->bits        = stage_bits;
    return ESP_OK;
}

static void sub_dispatch_frame(struct audio_capture_subscriber *s, const uint8_t *hub_frame, size_t hub_bytes, uint32_t hub_samples, uint8_t hub_channels, uint8_t hub_bits)
{
    const uint8_t *cur = hub_frame;
    size_t         cur_bytes = hub_bytes;
    uint32_t       cur_samples = hub_samples;
    uint8_t        cur_channels = hub_channels;
    uint8_t        cur_bits = hub_bits;

    if (s->rate_cvt) {
        uint32_t out_samples = s->rate_max_samples;
        esp_ae_err_t aerr = esp_ae_rate_cvt_process(s->rate_cvt, (esp_ae_sample_t)cur, cur_samples, (esp_ae_sample_t)s->rate_buf, &out_samples);
        if (aerr != ESP_AE_ERR_OK) return;
        cur = s->rate_buf;
        cur_samples = out_samples;
        cur_bytes = (size_t)out_samples * cur_channels * (cur_bits / 8);
    }
    if (s->ch_cvt) {
        esp_ae_err_t aerr = esp_ae_ch_cvt_process(s->ch_cvt, cur_samples, (esp_ae_sample_t)cur, (esp_ae_sample_t)s->ch_buf);
        if (aerr != ESP_AE_ERR_OK) return;
        cur = s->ch_buf;
        cur_channels = s->fmt.channels;
        cur_bytes = (size_t)cur_samples * cur_channels * (cur_bits / 8);
    }
    if (s->bit_cvt) {
        esp_ae_err_t aerr = esp_ae_bit_cvt_process(s->bit_cvt, cur_samples, (esp_ae_sample_t)cur, (esp_ae_sample_t)s->bit_buf);
        if (aerr != ESP_AE_ERR_OK) return;
        cur = s->bit_buf;
        cur_bits = s->fmt.bits;
        cur_bytes = (size_t)cur_samples * cur_channels * (cur_bits / 8);
    }

    (void)capture_ring_send_drop_new(s, cur, cur_bytes);
}

static void capture_task(void *arg)
{
    struct audio_capture_t *h = (struct audio_capture_t *)arg;
    while (h->running) {
        /* Snapshot open state under the mutex; the (potentially long)
         * codec read runs unlocked to avoid stalling _open_subscriber. */
        xSemaphoreTake(h->mtx, portMAX_DELAY);
        bool any_open = false;
        for (int i = 0; i < AUDIO_CAPTURE_SUB_ROLE_COUNT; ++i) {
            if (h->subs[i].open) { any_open = true; break; }
        }
        xSemaphoreGive(h->mtx);

        if (!any_open) {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_CAPTURE_IDLE_DELAY_MS));
            continue;
        }

        int rret = esp_codec_dev_read(h->codec_dev, h->hub_frame, (int)h->frame_bytes);
        if (rret != ESP_CODEC_DEV_OK) {
            ESP_LOGD(TAG, "codec_dev_read=%d", rret);
            vTaskDelay(pdMS_TO_TICKS(h->cfg.frame_ms));
            continue;
        }

        xSemaphoreTake(h->mtx, portMAX_DELAY);
        for (int i = 0; i < AUDIO_CAPTURE_SUB_ROLE_COUNT; ++i) {
            struct audio_capture_subscriber *s = &h->subs[i];
            if (!s->open) continue;
            if (s->rate_cvt == NULL && s->ch_cvt == NULL && s->bit_cvt == NULL) {
                (void)capture_ring_send_drop_new(s, h->hub_frame, h->frame_bytes);
            } else {
                sub_dispatch_frame(s, h->hub_frame, h->frame_bytes, h->samples_per_frame, h->cfg.channels, h->cfg.bits);
            }
        }
        xSemaphoreGive(h->mtx);
    }
    h->task = NULL;
    xSemaphoreGive(h->task_done);
    vTaskDelete(NULL);
}

static void capture_free_all(struct audio_capture_t *h)
{
    if (h == NULL) return;
    for (int i = 0; i < AUDIO_CAPTURE_SUB_ROLE_COUNT; ++i) {
        struct audio_capture_subscriber *s = &h->subs[i];
        sub_reset_conv(s);
        capture_delete_ring(s);
        if (s->io_mtx) { vSemaphoreDelete(s->io_mtx); s->io_mtx = NULL; }
        if (s->owner_tag) { free(s->owner_tag); s->owner_tag = NULL; }
        s->open = false;
    }
    if (h->hub_frame) {
        audio_mem_free(h->hub_frame);
        h->hub_frame = NULL;
    }
    if (h->codec_open && h->codec_dev) {
        esp_codec_dev_close(h->codec_dev);
        h->codec_open = false;
    }
    if (h->dev_lease) {
        claw_hw_release(h->dev_lease);
        h->dev_lease = NULL;
    }
    if (h->mtx) {
        vSemaphoreDelete(h->mtx);
        h->mtx = NULL;
    }
    if (h->task_done) {
        vSemaphoreDelete(h->task_done);
        h->task_done = NULL;
    }
    free(h);
}

esp_err_t audio_capture_start(const audio_capture_config_t *config,
                              audio_capture_handle_t *out_capture)
{
    if (config == NULL || out_capture == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct audio_capture_t *h = (struct audio_capture_t *)calloc(1, sizeof(*h));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }
    h->cfg = *config;
    capture_apply_defaults(&h->cfg);
    h->input_gain_db = AUDIO_CAPTURE_DEFAULT_INPUT_GAIN_DB;

    h->mtx = xSemaphoreCreateMutex();
    if (h->mtx == NULL) { capture_free_all(h); return ESP_ERR_NO_MEM; }
    h->task_done = xSemaphoreCreateBinary();
    if (h->task_done == NULL) { capture_free_all(h); return ESP_ERR_NO_MEM; }

    for (int i = 0; i < AUDIO_CAPTURE_SUB_ROLE_COUNT; ++i) {
        h->subs[i].role = (audio_capture_subscriber_role_t)i;
        h->subs[i].parent = h;
        h->subs[i].io_mtx = xSemaphoreCreateMutex();
        if (h->subs[i].io_mtx == NULL) { capture_free_all(h); return ESP_ERR_NO_MEM; }
    }

    void *dev_handle = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(AUDIO_CAPTURE_CODEC_DEVICE_NAME, &dev_handle);
    if (err != ESP_OK || dev_handle == NULL) {
        ESP_LOGE(TAG, "board device '%s' not found (err=0x%x)", AUDIO_CAPTURE_CODEC_DEVICE_NAME, (unsigned)err);
        capture_free_all(h);
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    h->codec_handles = (dev_audio_codec_handles_t *)dev_handle;
    h->codec_dev = h->codec_handles->codec_dev;
    if (h->codec_dev == NULL) {
        ESP_LOGE(TAG, "board device '%s' has no codec_dev handle", AUDIO_CAPTURE_CODEC_DEVICE_NAME);
        capture_free_all(h);
        return ESP_ERR_INVALID_STATE;
    }

    (void)claw_hw_key_device(h->device_key, sizeof(h->device_key),
                             AUDIO_CAPTURE_CODEC_DEVICE_NAME);
    claw_hw_claim_config_t claim = {
        .resource  = h->device_key,
        .owner_tag = AUDIO_CAPTURE_OWNER_TAG,
        .mode      = CLAW_HW_MODE_EXCLUSIVE,
    };
    err = claw_hw_claim(&claim, &h->dev_lease);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claim %s failed: 0x%x", h->device_key, (unsigned)err);
        capture_free_all(h);
        return err;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate      = h->cfg.sample_rate,
        .channel          = h->cfg.channels,
        .bits_per_sample  = h->cfg.bits,
    };
    int cret = esp_codec_dev_open(h->codec_dev, &fs);
    if (cret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec_dev_open failed: %d", cret);
        capture_free_all(h);
        return ESP_ERR_INVALID_STATE;
    }
    h->codec_open = true;

    err = capture_apply_uac_format(h);
    if (err != ESP_OK) {
        capture_free_all(h);
        return err;
    }

    h->bytes_per_frame   = (size_t)h->cfg.channels * (h->cfg.bits / 8);
    h->samples_per_frame = (h->cfg.sample_rate * h->cfg.frame_ms) / 1000;
    if (h->samples_per_frame == 0) h->samples_per_frame = 1;
    h->frame_bytes       = h->samples_per_frame * h->bytes_per_frame;
    if (h->frame_bytes == 0) { capture_free_all(h); return ESP_ERR_INVALID_ARG; }

    h->hub_frame = audio_mem_aligned_alloc(16, h->frame_bytes);
    if (h->hub_frame == NULL) { capture_free_all(h); return ESP_ERR_NO_MEM; }

    /* Apply mic gain after open because esp_codec_dev_open() restores its cached default input settings. */
    if (h->input_gain_db > 0.0f) {
        cret = esp_codec_dev_set_in_gain(h->codec_dev, h->input_gain_db);
        if (cret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "set default input_gain=%d dB failed: %d", (int)h->input_gain_db, cret);
        }
    }

    h->running = true;
    BaseType_t tres = xTaskCreate(capture_task, "audio_capture", AUDIO_CAPTURE_TASK_STACK, h, AUDIO_CAPTURE_TASK_PRIO, &h->task);
    if (tres != pdPASS) {
        h->running = false;
        capture_free_all(h);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "started device=%s rate=%" PRIu32 " ch=%u bits=%u frame_ms=%" PRIu32 " input_gain=%d dB",
             AUDIO_CAPTURE_CODEC_DEVICE_NAME, h->cfg.sample_rate,
             (unsigned)h->cfg.channels, (unsigned)h->cfg.bits,
             h->cfg.frame_ms, (int)h->input_gain_db);
    *out_capture = h;
    return ESP_OK;
}

esp_err_t audio_capture_stop(audio_capture_handle_t capture)
{
    if (capture == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    capture->running = false;
    xSemaphoreTake(capture->task_done, portMAX_DELAY);
    capture_free_all(capture);
    return ESP_OK;
}

esp_err_t audio_capture_set_input_gain(audio_capture_handle_t capture, float gain_db)
{
    if (capture == NULL || !capture_gain_is_valid(gain_db)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (capture->codec_dev == NULL || !capture->codec_open) {
        ESP_LOGE(TAG, "set input gain failed: codec is not open");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(capture->mtx, portMAX_DELAY);
    int cret = esp_codec_dev_set_in_gain(capture->codec_dev, gain_db);
    if (cret != ESP_CODEC_DEV_OK) {
        xSemaphoreGive(capture->mtx);
        ESP_LOGE(TAG, "set input_gain=%d dB failed: %d", (int)gain_db, cret);
        return ESP_FAIL;
    }
    capture->input_gain_db = gain_db;
    xSemaphoreGive(capture->mtx);
    return ESP_OK;
}

esp_err_t audio_capture_get_input_gain(audio_capture_handle_t capture, float *out_gain_db)
{
    if (capture == NULL || out_gain_db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (capture->codec_dev == NULL || !capture->codec_open) {
        ESP_LOGE(TAG, "get input gain failed: codec is not open");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(capture->mtx, portMAX_DELAY);
    float gain_db = capture->input_gain_db;
    int cret = esp_codec_dev_get_in_gain(capture->codec_dev, &gain_db);
    if (cret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "get input_gain failed: %d, using cached gain", cret);
        gain_db = capture->input_gain_db;
    } else {
        capture->input_gain_db = gain_db;
    }
    xSemaphoreGive(capture->mtx);

    *out_gain_db = gain_db;
    return ESP_OK;
}

esp_err_t audio_capture_open_subscriber(audio_capture_handle_t capture, audio_capture_subscriber_role_t role, const audio_capture_sub_format_t *fmt, const char *owner_tag, audio_capture_sub_handle_t *out_sub)
{
    if (capture == NULL || out_sub == NULL ||
        owner_tag == NULL || owner_tag[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (role != AUDIO_CAPTURE_SUB_SYSTEM && role != AUDIO_CAPTURE_SUB_APP) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(capture->mtx, portMAX_DELAY);
    struct audio_capture_subscriber *s = &capture->subs[role];
    if (s->open) {
        xSemaphoreGive(capture->mtx);
        ESP_LOGE(TAG, "role %d already open (owner=%s)", (int)role, s->owner_tag ? s->owner_tag : "?");
        return ESP_ERR_INVALID_STATE;
    }

    audio_capture_sub_format_t tgt = {0};
    if (fmt) tgt = *fmt;
    /* Inherit hub defaults for any 0 field. */
    if (tgt.sample_rate == 0) tgt.sample_rate = capture->cfg.sample_rate;
    if (tgt.channels    == 0) tgt.channels    = capture->cfg.channels;
    if (tgt.bits        == 0) tgt.bits        = capture->cfg.bits;
    s->fmt = tgt;

    s->owner_tag = strdup(owner_tag);
    if (s->owner_tag == NULL) {
        xSemaphoreGive(capture->mtx);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    if (!fmt_matches_hub(&s->fmt, &capture->cfg)) {
        err = sub_build_conv(s, capture);
        if (err != ESP_OK) {
            sub_reset_conv(s);
            free(s->owner_tag);
            s->owner_tag = NULL;
            xSemaphoreGive(capture->mtx);
            return err;
        }
    }

    /* Ring is sized in target frames (post-conversion). */
    size_t sub_frame_bytes = (size_t)((s->fmt.sample_rate * capture->cfg.frame_ms) / 1000)
                             * s->fmt.channels * (s->fmt.bits / 8);
    if (sub_frame_bytes == 0) sub_frame_bytes = capture->frame_bytes;
    size_t ring_bytes = sub_frame_bytes * capture->cfg.ring_frames;
    s->ring = audio_mem_stream_buffer_create(ring_bytes, 1);
    if (s->ring == NULL) {
        sub_reset_conv(s);
        free(s->owner_tag);
        s->owner_tag = NULL;
        xSemaphoreGive(capture->mtx);
        return ESP_ERR_NO_MEM;
    }

    s->dropped_bytes = 0;
    s->open = true;
    ESP_LOGI(TAG, "subscriber opened role=%d owner=%s ring=%u bytes", (int)role, owner_tag, (unsigned)ring_bytes);
    xSemaphoreGive(capture->mtx);

    *out_sub = s;
    return ESP_OK;
}

esp_err_t audio_capture_close_subscriber(audio_capture_sub_handle_t sub)
{
    if (sub == NULL || sub->parent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct audio_capture_t *h = sub->parent;
    xSemaphoreTake(h->mtx, portMAX_DELAY);
    if (!sub->open) {
        xSemaphoreGive(h->mtx);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(sub->io_mtx, portMAX_DELAY);
    sub->open = false;
    capture_delete_ring(sub);
    sub_reset_conv(sub);
    if (sub->owner_tag) { free(sub->owner_tag); sub->owner_tag = NULL; }
    xSemaphoreGive(sub->io_mtx);
    xSemaphoreGive(h->mtx);
    return ESP_OK;
}

esp_err_t audio_capture_sub_flush(audio_capture_sub_handle_t sub)
{
    if (sub == NULL || sub->parent == NULL || sub->io_mtx == NULL) return ESP_ERR_INVALID_ARG;
    struct audio_capture_t *h = sub->parent;
    xSemaphoreTake(h->mtx, portMAX_DELAY);
    xSemaphoreTake(sub->io_mtx, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!sub->open || sub->ring == NULL) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        (void)xStreamBufferReset(sub->ring);
    }
    xSemaphoreGive(sub->io_mtx);
    xSemaphoreGive(h->mtx);
    return err;
}

size_t audio_capture_sub_read(audio_capture_sub_handle_t sub, void *pcm, size_t bytes, uint32_t timeout_ms)
{
    if (sub == NULL || pcm == NULL || bytes == 0 || sub->parent == NULL || sub->io_mtx == NULL) {
        return 0;
    }
    TickType_t started = xTaskGetTickCount();
    for (;;) {
        xSemaphoreTake(sub->io_mtx, portMAX_DELAY);
        if (!sub->open || sub->ring == NULL) {
            xSemaphoreGive(sub->io_mtx);
            return 0;
        }
        uint32_t wait_ms = timeout_ms == UINT32_MAX ? AUDIO_CAPTURE_IO_SLICE_MS : timeout_ms;
        if (wait_ms > AUDIO_CAPTURE_IO_SLICE_MS) wait_ms = AUDIO_CAPTURE_IO_SLICE_MS;
        size_t got = xStreamBufferReceive(sub->ring, pcm, bytes, pdMS_TO_TICKS(wait_ms));
        xSemaphoreGive(sub->io_mtx);
        if (got > 0 || timeout_ms == 0) return got;
        if (timeout_ms != UINT32_MAX && pdTICKS_TO_MS(xTaskGetTickCount() - started) >= timeout_ms) {
            return 0;
        }
    }
}

esp_err_t audio_capture_sub_info(audio_capture_sub_handle_t sub, uint32_t *sample_rate, uint8_t *channels, uint8_t *bits)
{
    if (sub == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate) *sample_rate = sub->fmt.sample_rate;
    if (channels)    *channels    = sub->fmt.channels;
    if (bits)        *bits        = sub->fmt.bits;
    return ESP_OK;
}

esp_err_t audio_capture_sub_get_dropped_bytes(audio_capture_sub_handle_t sub,
                                              uint64_t *dropped_bytes)
{
    if (sub == NULL || dropped_bytes == NULL || sub->parent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct audio_capture_t *h = sub->parent;
    xSemaphoreTake(h->mtx, portMAX_DELAY);
    *dropped_bytes = sub->dropped_bytes;
    xSemaphoreGive(h->mtx);
    return ESP_OK;
}
