/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* audio_mixer — see include/audio_mixer.h. */

#include "audio_mixer.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "esp_ae_mixer.h"
#include "esp_board_manager.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_log.h"

#include "claw_hw_registry.h"
#include "dev_audio_codec.h"
#include "audio_memory.h"

static const char *TAG = "audio_mixer";

#define AUDIO_MIXER_TASK_PRIO         10
#define AUDIO_MIXER_TASK_STACK        4096
#define AUDIO_MIXER_MIXER_OWNER_TAG   "mixer/system"
#define AUDIO_MIXER_TRACK_ROLE_COUNT  2
#define AUDIO_MIXER_CODEC_DEVICE_NAME "audio_dac"
#define AUDIO_MIXER_RING_FRAMES       8
/* Cap on how long a producer will block if the mixer task stalls. */
#define AUDIO_MIXER_WRITE_TIMEOUT_MS  500
#define AUDIO_MIXER_IO_RETRY_MS        5
/* |sample| below this counts as silence (s16 only). */
#define AUDIO_MIXER_SILENCE_THRESHOLD 128

/* UAC virtual registers are optional and identify themselves by magic. */
#define AUDIO_MIXER_UAC_FORMAT_MAGIC_REG       0x7AC0
#define AUDIO_MIXER_UAC_FORMAT_SAMPLE_RATE_REG 0x7AC1
#define AUDIO_MIXER_UAC_FORMAT_CHANNELS_REG    0x7AC2
#define AUDIO_MIXER_UAC_FORMAT_BITS_REG        0x7AC3
#define AUDIO_MIXER_UAC_FORMAT_MAGIC_VALUE     0x55414346

struct audio_mixer_track {
    audio_mixer_track_role_t role;
    StreamBufferHandle_t     ring;
    SemaphoreHandle_t        io_mtx;
    char                    *owner_tag;
    bool                     open;
    struct audio_mixer_t    *parent;
};

struct audio_mixer_t {
    audio_mixer_config_t       cfg;
    dev_audio_codec_handles_t *codec_handles;
    esp_codec_dev_handle_t     codec_dev;
    claw_hw_lease_handle_t     dev_lease;
    char                       device_key[48];

    esp_ae_mixer_handle_t      ae_mixer;
    esp_ae_mixer_info_t        src_info[AUDIO_MIXER_TRACK_ROLE_COUNT];

    size_t                     bytes_per_frame;
    uint32_t                   samples_per_frame;
    size_t                     frame_bytes;

    uint8_t                   *scratch_system;
    uint8_t                   *scratch_app;
    uint8_t                   *scratch_out;

    struct audio_mixer_track   tracks[AUDIO_MIXER_TRACK_ROLE_COUNT];

    bool                       app_ducked;
    uint32_t                   silence_ms_accum;

    SemaphoreHandle_t          mtx;
    SemaphoreHandle_t          task_done;
    TaskHandle_t               task;
    volatile bool              running;
};

static void mixer_apply_defaults(audio_mixer_config_t *cfg)
{
    if (cfg->sample_rate == 0) cfg->sample_rate = 16000;
    if (cfg->channels == 0)    cfg->channels = 1;
    if (cfg->bits == 0)        cfg->bits = 16;
    if (cfg->frame_ms == 0)    cfg->frame_ms = 20;
    if (cfg->system_full_gain == 0.0f) cfg->system_full_gain = 1.0f;
    if (cfg->app_full_gain == 0.0f)    cfg->app_full_gain = 1.0f;
    if (cfg->app_ducked_gain == 0.0f)  cfg->app_ducked_gain = 0.3f;
    if (cfg->duck_release_ms == 0)     cfg->duck_release_ms = 300;
    if (cfg->output_volume <= 0 || cfg->output_volume > 100) {
        cfg->output_volume = 80;
    }
}

static esp_err_t mixer_apply_uac_format(struct audio_mixer_t *m)
{
    int magic = 0;
    int ret = esp_codec_dev_read_reg(m->codec_dev, AUDIO_MIXER_UAC_FORMAT_MAGIC_REG, &magic);
    if (ret != ESP_CODEC_DEV_OK || magic != AUDIO_MIXER_UAC_FORMAT_MAGIC_VALUE) {
        return ESP_OK;
    }

    int sample_rate = 0;
    int channels = 0;
    int bits = 0;
    ret = esp_codec_dev_read_reg(m->codec_dev, AUDIO_MIXER_UAC_FORMAT_SAMPLE_RATE_REG, &sample_rate);
    if (ret == ESP_CODEC_DEV_OK) ret = esp_codec_dev_read_reg(m->codec_dev, AUDIO_MIXER_UAC_FORMAT_CHANNELS_REG, &channels);
    if (ret == ESP_CODEC_DEV_OK) ret = esp_codec_dev_read_reg(m->codec_dev, AUDIO_MIXER_UAC_FORMAT_BITS_REG, &bits);
    if (ret != ESP_CODEC_DEV_OK || sample_rate <= 0 || channels <= 0 || bits <= 0 || bits % 8 != 0) {
        ESP_LOGE(TAG, "invalid UAC format ret=%d rate=%d ch=%d bits=%d", ret, sample_rate, channels, bits);
        return ESP_ERR_INVALID_STATE;
    }

    m->cfg.sample_rate = (uint32_t)sample_rate;
    m->cfg.channels = (uint8_t)channels;
    m->cfg.bits = (uint8_t)bits;
    ESP_LOGI(TAG, "using UAC format rate=%d ch=%d bits=%d", sample_rate, channels, bits);
    return ESP_OK;
}

static void mixer_delete_ring(struct audio_mixer_track *track)
{
    if (!track || !track->ring) {
        return;
    }
    audio_mem_stream_buffer_delete(track->ring);
    track->ring = NULL;
}

static bool frame_has_nonsilence_s16(const int16_t *pcm, uint32_t samples)
{
    for (uint32_t i = 0; i < samples; ++i) {
        int16_t v = pcm[i];
        int32_t abs_v = v < 0 ? -(int32_t)v : (int32_t)v;
        if (abs_v > AUDIO_MIXER_SILENCE_THRESHOLD) {
            return true;
        }
    }
    return false;
}

static bool frame_has_nonsilence(const void *pcm, size_t bytes, uint8_t bits)
{
    /* Non-16-bit is conservatively reported as non-silent, biasing ducking
     * to be more aggressive rather than missing a system prompt. */
    if (bits == 16) {
        return frame_has_nonsilence_s16((const int16_t *)pcm, (uint32_t)(bytes / sizeof(int16_t)));
    }
    return true;
}

/* Zero-fills tail when the ring has fewer than frame_bytes ready. */
static bool mixer_drain_track(struct audio_mixer_t *m, audio_mixer_track_role_t role, uint8_t *scratch, size_t frame_bytes)
{
    struct audio_mixer_track *t = &m->tracks[role];
    size_t got = 0;
    xSemaphoreTake(t->io_mtx, portMAX_DELAY);
    if (t->open && t->ring != NULL) {
        got = xStreamBufferReceive(t->ring, scratch, frame_bytes, 0);
    }
    xSemaphoreGive(t->io_mtx);
    if (got < frame_bytes) {
        memset(scratch + got, 0, frame_bytes - got);
    }
    return got > 0;
}

static void mixer_task(void *arg)
{
    struct audio_mixer_t *m = (struct audio_mixer_t *)arg;
    const uint32_t frame_ms = m->cfg.frame_ms;

    while (m->running) {
        bool sys_had_data = mixer_drain_track(m, AUDIO_MIXER_TRACK_SYSTEM, m->scratch_system, m->frame_bytes);
        (void)mixer_drain_track(m, AUDIO_MIXER_TRACK_APP, m->scratch_app, m->frame_bytes);

        esp_ae_sample_t in_samples[AUDIO_MIXER_TRACK_ROLE_COUNT] = {
            m->scratch_system,
            m->scratch_app,
        };
        esp_ae_err_t aerr = esp_ae_mixer_process(m->ae_mixer, m->samples_per_frame, in_samples, m->scratch_out);
        if (aerr != ESP_AE_ERR_OK) {
            ESP_LOGE(TAG, "mixer_process failed: %d", (int)aerr);
            vTaskDelay(pdMS_TO_TICKS(frame_ms));
            continue;
        }

        int wret = esp_codec_dev_write(m->codec_dev, m->scratch_out, (int)m->frame_bytes);
        if (wret != ESP_CODEC_DEV_OK) {
            ESP_LOGD(TAG, "codec_dev_write=%d", wret);
        }

        bool sys_nonsilent = sys_had_data && frame_has_nonsilence(m->scratch_system, m->frame_bytes, m->cfg.bits);
        if (sys_nonsilent) {
            m->silence_ms_accum = 0;
            if (!m->app_ducked) {
                m->app_ducked = true;
                esp_ae_mixer_set_mode(m->ae_mixer, 1, ESP_AE_MIXER_MODE_FADE_UPWARD);
            }
        } else {
            m->silence_ms_accum += frame_ms;
            if (m->app_ducked && m->silence_ms_accum >= m->cfg.duck_release_ms) {
                m->app_ducked = false;
                esp_ae_mixer_set_mode(m->ae_mixer, 1, ESP_AE_MIXER_MODE_FADE_DOWNWARD);
            }
        }
    }

    m->task = NULL;
    xSemaphoreGive(m->task_done);
    vTaskDelete(NULL);
}

static esp_err_t mixer_alloc_scratch(struct audio_mixer_t *m)
{
    m->scratch_system = audio_mem_aligned_alloc(16, m->frame_bytes);
    m->scratch_app    = audio_mem_aligned_alloc(16, m->frame_bytes);
    m->scratch_out    = audio_mem_aligned_alloc(16, m->frame_bytes);
    if (m->scratch_system == NULL || m->scratch_app == NULL || m->scratch_out == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(m->scratch_system, 0, m->frame_bytes);
    memset(m->scratch_app,    0, m->frame_bytes);
    memset(m->scratch_out,    0, m->frame_bytes);
    return ESP_OK;
}

static void mixer_free_scratch(struct audio_mixer_t *m)
{
    if (m->scratch_system) { audio_mem_free(m->scratch_system); m->scratch_system = NULL; }
    if (m->scratch_app)    { audio_mem_free(m->scratch_app);    m->scratch_app = NULL; }
    if (m->scratch_out)    { audio_mem_free(m->scratch_out);    m->scratch_out = NULL; }
}

static void mixer_free_all(struct audio_mixer_t *m)
{
    if (m == NULL) return;
    if (m->task != NULL) {
        m->task = NULL; /* task self-deletes when `running` goes false */
    }
    for (int i = 0; i < AUDIO_MIXER_TRACK_ROLE_COUNT; ++i) {
        mixer_delete_ring(&m->tracks[i]);
        if (m->tracks[i].io_mtx) { vSemaphoreDelete(m->tracks[i].io_mtx); m->tracks[i].io_mtx = NULL; }
        if (m->tracks[i].owner_tag) {
            free(m->tracks[i].owner_tag);
            m->tracks[i].owner_tag = NULL;
        }
        m->tracks[i].open = false;
    }
    mixer_free_scratch(m);
    if (m->ae_mixer) {
        esp_ae_mixer_close(m->ae_mixer);
        m->ae_mixer = NULL;
    }
    if (m->dev_lease) {
        claw_hw_release(m->dev_lease);
        m->dev_lease = NULL;
    }
    if (m->codec_dev) {
        esp_codec_dev_close(m->codec_dev);
        m->codec_dev = NULL;
    }
    if (m->mtx) {
        vSemaphoreDelete(m->mtx);
        m->mtx = NULL;
    }
    if (m->task_done) {
        vSemaphoreDelete(m->task_done);
        m->task_done = NULL;
    }
    free(m);
}

esp_err_t audio_mixer_start(const audio_mixer_config_t *config,
                            audio_mixer_handle_t *out_mixer)
{
    if (config == NULL || out_mixer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct audio_mixer_t *m = (struct audio_mixer_t *)calloc(1, sizeof(*m));
    if (m == NULL) {
        return ESP_ERR_NO_MEM;
    }
    m->cfg = *config;
    mixer_apply_defaults(&m->cfg);

    m->mtx = xSemaphoreCreateMutex();
    if (m->mtx == NULL) {
        mixer_free_all(m);
        return ESP_ERR_NO_MEM;
    }
    m->task_done = xSemaphoreCreateBinary();
    if (m->task_done == NULL) {
        mixer_free_all(m);
        return ESP_ERR_NO_MEM;
    }

    void *dev_handle = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(AUDIO_MIXER_CODEC_DEVICE_NAME, &dev_handle);
    if (err != ESP_OK || dev_handle == NULL) {
        ESP_LOGE(TAG, "board device '%s' not found (err=0x%x)", AUDIO_MIXER_CODEC_DEVICE_NAME, (unsigned)err);
        mixer_free_all(m);
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    m->codec_handles = (dev_audio_codec_handles_t *)dev_handle;
    m->codec_dev = m->codec_handles->codec_dev;
    if (m->codec_dev == NULL) {
        ESP_LOGE(TAG, "board device '%s' has no codec_dev handle", AUDIO_MIXER_CODEC_DEVICE_NAME);
        mixer_free_all(m);
        return ESP_ERR_INVALID_STATE;
    }

    (void)claw_hw_key_device(m->device_key, sizeof(m->device_key), AUDIO_MIXER_CODEC_DEVICE_NAME);
    claw_hw_claim_config_t claim = {
        .resource  = m->device_key,
        .owner_tag = AUDIO_MIXER_MIXER_OWNER_TAG,
        .mode      = CLAW_HW_MODE_EXCLUSIVE,
    };
    err = claw_hw_claim(&claim, &m->dev_lease);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claim %s failed: 0x%x", m->device_key, (unsigned)err);
        mixer_free_all(m);
        return err;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate      = m->cfg.sample_rate,
        .channel          = m->cfg.channels,
        .bits_per_sample  = m->cfg.bits,
    };
    int cret = esp_codec_dev_open(m->codec_dev, &fs);
    if (cret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec_dev_open failed: %d", cret);
        /* Suppress double-close in cleanup. */
        m->codec_dev = NULL;
        mixer_free_all(m);
        return ESP_ERR_INVALID_STATE;
    }

    err = mixer_apply_uac_format(m);
    if (err != ESP_OK) {
        mixer_free_all(m);
        return err;
    }

    m->bytes_per_frame   = (size_t)m->cfg.channels * (m->cfg.bits / 8);
    m->samples_per_frame = (m->cfg.sample_rate * m->cfg.frame_ms) / 1000;
    if (m->samples_per_frame == 0) m->samples_per_frame = 1;
    m->frame_bytes       = m->samples_per_frame * m->bytes_per_frame;
    if (m->frame_bytes == 0) {
        mixer_free_all(m);
        return ESP_ERR_INVALID_ARG;
    }

    /* Most codecs come out of _open muted; apply the initial DAC volume
     * before the mixer task starts writing. */
    cret = esp_codec_dev_set_out_vol(m->codec_dev, m->cfg.output_volume);
    if (cret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "set_out_vol=%d failed: %d", m->cfg.output_volume, cret);
    }

    /* esp_ae_mixer requires transit_time > 0 on every source. */
    m->src_info[0].weight1      = m->cfg.system_full_gain;
    m->src_info[0].weight2      = m->cfg.system_full_gain;
    m->src_info[0].transit_time = m->cfg.duck_release_ms;
    m->src_info[1].weight1      = m->cfg.app_full_gain;
    m->src_info[1].weight2      = m->cfg.app_ducked_gain;
    m->src_info[1].transit_time = m->cfg.duck_release_ms;

    esp_ae_mixer_cfg_t ae_cfg = {
        .sample_rate      = m->cfg.sample_rate,
        .channel          = m->cfg.channels,
        .bits_per_sample  = m->cfg.bits,
        .src_num          = AUDIO_MIXER_TRACK_ROLE_COUNT,
        .src_info         = m->src_info,
    };
    esp_ae_err_t aerr = esp_ae_mixer_open(&ae_cfg, &m->ae_mixer);
    if (aerr != ESP_AE_ERR_OK || m->ae_mixer == NULL) {
        ESP_LOGE(TAG, "esp_ae_mixer_open failed: %d", (int)aerr);
        mixer_free_all(m);
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < AUDIO_MIXER_TRACK_ROLE_COUNT; ++i) {
        m->tracks[i].role = (audio_mixer_track_role_t)i;
        m->tracks[i].parent = m;
        m->tracks[i].io_mtx = xSemaphoreCreateMutex();
        if (m->tracks[i].io_mtx == NULL) {
            mixer_free_all(m);
            return ESP_ERR_NO_MEM;
        }
        m->tracks[i].ring = audio_mem_stream_buffer_create(m->frame_bytes * AUDIO_MIXER_RING_FRAMES, 1);
        if (m->tracks[i].ring == NULL) {
            mixer_free_all(m);
            return ESP_ERR_NO_MEM;
        }
    }
    err = mixer_alloc_scratch(m);
    if (err != ESP_OK) {
        mixer_free_all(m);
        return err;
    }

    m->running = true;
    BaseType_t tres = xTaskCreate(mixer_task, "audio_mixer", AUDIO_MIXER_TASK_STACK, m, AUDIO_MIXER_TASK_PRIO, &m->task);
    if (tres != pdPASS) {
        m->running = false;
        m->task = NULL;
        mixer_free_all(m);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started device=%s rate=%" PRIu32 " ch=%u bits=%u frame_ms=%" PRIu32, AUDIO_MIXER_CODEC_DEVICE_NAME, m->cfg.sample_rate, (unsigned)m->cfg.channels, (unsigned)m->cfg.bits, m->cfg.frame_ms);
    *out_mixer = m;
    return ESP_OK;
}

esp_err_t audio_mixer_stop(audio_mixer_handle_t mixer)
{
    if (mixer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    mixer->running = false;
    xSemaphoreTake(mixer->task_done, portMAX_DELAY);
    mixer_free_all(mixer);
    return ESP_OK;
}

esp_err_t audio_mixer_set_output_volume(audio_mixer_handle_t mixer, int percent)
{
    if (mixer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mixer->codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int cret = esp_codec_dev_set_out_vol(mixer->codec_dev, percent);
    if (cret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "set_out_vol=%d failed: %d", percent, cret);
        return ESP_FAIL;
    }
    mixer->cfg.output_volume = percent;
    return ESP_OK;
}

esp_err_t audio_mixer_get_output_volume(audio_mixer_handle_t mixer, int *out_percent)
{
    if (mixer == NULL || out_percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mixer->codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int vol = 0;
    if (esp_codec_dev_get_out_vol(mixer->codec_dev, &vol) != ESP_CODEC_DEV_OK) {
        /* Some codecs have no getter; fall back to the shadow copy. */
        vol = mixer->cfg.output_volume;
    }
    *out_percent = vol;
    return ESP_OK;
}

esp_err_t audio_mixer_open_track(audio_mixer_handle_t mixer, audio_mixer_track_role_t role, const char *owner_tag, audio_mixer_track_handle_t *out_track)
{
    if (mixer == NULL || out_track == NULL ||
        owner_tag == NULL || owner_tag[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (role != AUDIO_MIXER_TRACK_SYSTEM && role != AUDIO_MIXER_TRACK_APP) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(mixer->mtx, portMAX_DELAY);
    struct audio_mixer_track *t = &mixer->tracks[role];
    if (t->open) {
        xSemaphoreGive(mixer->mtx);
        ESP_LOGE(TAG, "role %d already open (owner=%s)", (int)role, t->owner_tag ? t->owner_tag : "?");
        return ESP_ERR_INVALID_STATE;
    }
    t->owner_tag = strdup(owner_tag);
    if (t->owner_tag == NULL) {
        xSemaphoreGive(mixer->mtx);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(t->io_mtx, portMAX_DELAY);
    (void)xStreamBufferReset(t->ring);
    t->open = true;
    xSemaphoreGive(t->io_mtx);
    xSemaphoreGive(mixer->mtx);

    *out_track = t;
    return ESP_OK;
}

esp_err_t audio_mixer_close_track(audio_mixer_track_handle_t track)
{
    if (track == NULL || track->parent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct audio_mixer_t *m = track->parent;
    xSemaphoreTake(m->mtx, portMAX_DELAY);
    if (!track->open) {
        xSemaphoreGive(m->mtx);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(track->io_mtx, portMAX_DELAY);
    track->open = false;
    (void)xStreamBufferReset(track->ring);
    if (track->owner_tag) {
        free(track->owner_tag);
        track->owner_tag = NULL;
    }
    xSemaphoreGive(track->io_mtx);
    xSemaphoreGive(m->mtx);
    return ESP_OK;
}

size_t audio_mixer_track_write(audio_mixer_track_handle_t track,
                               const void *pcm, size_t bytes)
{
    if (track == NULL || pcm == NULL || bytes == 0 || track->parent == NULL || track->io_mtx == NULL) {
        return 0;
    }
    const uint8_t *src = (const uint8_t *)pcm;
    TickType_t started = xTaskGetTickCount();
    size_t total = 0;
    while (total < bytes) {
        xSemaphoreTake(track->io_mtx, portMAX_DELAY);
        if (!track->open || track->ring == NULL) {
            xSemaphoreGive(track->io_mtx);
            break;
        }
        total += xStreamBufferSend(track->ring, src + total, bytes - total, 0);
        xSemaphoreGive(track->io_mtx);
        if (total == bytes || pdTICKS_TO_MS(xTaskGetTickCount() - started) >= AUDIO_MIXER_WRITE_TIMEOUT_MS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(AUDIO_MIXER_IO_RETRY_MS));
    }
    return total;
}

esp_err_t audio_mixer_track_flush(audio_mixer_track_handle_t track)
{
    if (track == NULL || track->parent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(track->io_mtx, portMAX_DELAY);
    bool open = track->open && track->ring != NULL;
    xSemaphoreGive(track->io_mtx);
    if (!open) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t frame_ms = track->parent->cfg.frame_ms;
    const int max_iters = 200; /* ~4 s cap at frame_ms = 20 ms */
    for (int i = 0; i < max_iters; ++i) {
        xSemaphoreTake(track->io_mtx, portMAX_DELAY);
        bool drained = !track->open || track->ring == NULL || xStreamBufferBytesAvailable(track->ring) == 0;
        xSemaphoreGive(track->io_mtx);
        if (drained) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(frame_ms > 0 ? frame_ms : 5));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t audio_mixer_track_stop(audio_mixer_track_handle_t track)
{
    if (track == NULL || track->parent == NULL || track->io_mtx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(track->io_mtx, portMAX_DELAY);
    if (track->ring) {
        (void)xStreamBufferReset(track->ring);
    }
    xSemaphoreGive(track->io_mtx);
    return ESP_OK;
}

esp_err_t audio_mixer_track_info(audio_mixer_track_handle_t track, uint32_t *sample_rate, uint8_t *channels, uint8_t *bits)
{
    if (track == NULL || track->parent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const audio_mixer_config_t *cfg = &track->parent->cfg;
    if (sample_rate) *sample_rate = cfg->sample_rate;
    if (channels)    *channels    = cfg->channels;
    if (bits)        *bits        = cfg->bits;
    return ESP_OK;
}
