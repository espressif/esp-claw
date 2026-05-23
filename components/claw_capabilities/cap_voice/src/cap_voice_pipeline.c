/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-SR V2 pipeline: AFE → WakeNet → VAD → MultiNet / Whisper fallback
 */
#include "cap_voice_pipeline.h"
#include "cap_voice_audio.h"
#include "cap_voice_whisper.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "cap_voice_pipeline";

#define FEED_TASK_STACK   4096
#define DETECT_TASK_STACK 6144
#define PIPELINE_TASK_PRI 5

/* Partition label for ESP-SR models stored in flash */
#define MODEL_PARTITION_LABEL "model"

static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t        *s_afe_data;
static esp_mn_iface_t           *s_mn_handle;
static model_iface_data_t       *s_mn_data;
static cap_voice_pipeline_config_t s_cfg;
static cap_voice_state_t         s_state = CAP_VOICE_STATE_IDLE;
static TaskHandle_t              s_feed_task;
static TaskHandle_t              s_detect_task;
static volatile bool             s_running;

#define RECORD_BUF_SAMPLES (CONFIG_CAP_VOICE_RECORD_MAX_MS * 16)
static int16_t  *s_record_buf;
static size_t    s_record_pos;

static void feed_task(void *arg)
{
    (void)arg;
    const int chunk = s_afe_handle->get_feed_chunksize(s_afe_data);
    int16_t *buf = heap_caps_malloc(chunk * sizeof(int16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "feed task: no memory for chunk buffer");
        vTaskDelete(NULL);
        return;
    }
    while (s_running) {
        size_t got = 0;
        esp_err_t err = cap_voice_audio_read(buf, chunk, &got);
        if (err == ESP_OK && got > 0) {
            s_afe_handle->feed(s_afe_data, buf);
        }
        if (s_state == CAP_VOICE_STATE_LISTENING &&
                s_record_pos + chunk < RECORD_BUF_SAMPLES) {
            memcpy(s_record_buf + s_record_pos, buf, chunk * sizeof(int16_t));
            s_record_pos += chunk;
        }
    }
    free(buf);
    vTaskDelete(NULL);
}

static void do_recognition(void)
{
    if (!s_mn_handle || !s_mn_data) {
        goto whisper_fallback;
    }
    const int mn_chunk = s_mn_handle->get_samp_chunksize(s_mn_data);
    if (mn_chunk <= 0) {
        ESP_LOGW(TAG, "MultiNet chunk size invalid");
        goto whisper_fallback;
    }
    bool found = false;
    for (size_t off = 0; off + mn_chunk <= s_record_pos && !found; off += mn_chunk) {
        esp_mn_state_t mn_state = s_mn_handle->detect(
            s_mn_data, s_record_buf + off);
        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *result = s_mn_handle->get_results(s_mn_data);
            if (result && result->num > 0) {
                float score = result->prob[0];
                ESP_LOGI(TAG, "MultiNet: cmd_id=%d score=%.2f text=%s",
                         result->command_id[0], score, result->string);
                if (score >= s_cfg.multinet_threshold && s_cfg.on_command) {
                    s_cfg.on_command(result->string, result->string, s_cfg.cb_ctx);
                    found = true;
                }
            }
        }
    }
    if (found) return;

whisper_fallback:
    ESP_LOGI(TAG, "Falling back to Whisper");
    char *transcript = NULL;
    cap_voice_whisper_config_t wcfg = {
        .api_key  = s_cfg.whisper_api_key,
        .base_url = s_cfg.whisper_base_url,
        .language = "ru",
    };
    if (cap_voice_whisper_transcribe(&wcfg, s_record_buf,
                                      s_record_pos, &transcript) == ESP_OK
        && transcript && s_cfg.on_transcript) {
        s_cfg.on_transcript(NULL, transcript, s_cfg.cb_ctx);
    }
    free(transcript);
}

static void detect_task(void *arg)
{
    (void)arg;
    int silence_ms = 0;
    while (s_running) {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!res) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (s_state == CAP_VOICE_STATE_IDLE) {
            if (res->wakeup_state == WAKENET_DETECTED) {
                ESP_LOGI(TAG, "Wake word detected!");
                s_state = CAP_VOICE_STATE_LISTENING;
                s_record_pos = 0;
                silence_ms = 0;
                if (s_cfg.on_wakeup) s_cfg.on_wakeup(s_cfg.cb_ctx);
            }
        } else if (s_state == CAP_VOICE_STATE_LISTENING) {
            if (res->vad_state == VAD_SILENCE) {
                silence_ms += 32;
            } else {
                silence_ms = 0;
            }
            if (silence_ms >= CONFIG_CAP_VOICE_VAD_SILENCE_MS ||
                    s_record_pos >= RECORD_BUF_SAMPLES) {
                ESP_LOGI(TAG, "Recording done: %u samples",
                         (unsigned)s_record_pos);
                s_state = CAP_VOICE_STATE_RECOGNIZING;
                do_recognition();
                s_state = CAP_VOICE_STATE_IDLE;
            }
        }
    }
    vTaskDelete(NULL);
}

esp_err_t cap_voice_pipeline_init(const cap_voice_pipeline_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;

    s_record_buf = heap_caps_malloc(RECORD_BUF_SAMPLES * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_record_buf) {
        ESP_LOGE(TAG, "no PSRAM for record buffer");
        return ESP_ERR_NO_MEM;
    }

    /* Load model list from flash partition */
    srmodel_list_t *models = esp_srmodel_init(MODEL_PARTITION_LABEL);
    if (!models) {
        ESP_LOGW(TAG, "no model partition '%s'; WakeNet/MultiNet unavailable",
                 MODEL_PARTITION_LABEL);
    }

    /* Init AFE (Audio Front-End with WakeNet + VAD) */
    afe_config_t *afe_cfg = afe_config_init("M", models,
                                             AFE_TYPE_SR,
                                             AFE_MODE_LOW_COST);
    if (!afe_cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        if (models) esp_srmodel_deinit(models);
        return ESP_FAIL;
    }
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    s_afe_handle = esp_afe_handle_from_config(afe_cfg);
    if (!s_afe_handle) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_cfg);
        if (models) esp_srmodel_deinit(models);
        return ESP_FAIL;
    }
    s_afe_data = s_afe_handle->create_from_config(afe_cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create failed");
        afe_config_free(afe_cfg);
        if (models) esp_srmodel_deinit(models);
        return ESP_FAIL;
    }
    afe_config_free(afe_cfg);
    ESP_LOGI(TAG, "AFE + WakeNet initialized");

    /* Init MultiNet (command recognition) */
    char *mn_name = (models) ?
        esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH) : NULL;
    if (mn_name) {
        s_mn_handle = esp_mn_handle_from_name(mn_name);
    }
    if (s_mn_handle) {
        s_mn_data = s_mn_handle->create(mn_name, 6000);
        esp_mn_commands_alloc(s_mn_handle, s_mn_data);
        esp_mn_commands_add(1, "stop");
        esp_mn_commands_add(2, "go forward");
        esp_mn_commands_add(3, "go back");
        esp_mn_commands_add(4, "turn left");
        esp_mn_commands_add(5, "turn right");
        esp_mn_commands_add(6, "open gripper");
        esp_mn_commands_add(7, "close gripper");
        esp_mn_commands_update();
        ESP_LOGI(TAG, "MultiNet initialized with 7 commands");
    } else {
        ESP_LOGW(TAG, "MultiNet not available; using Whisper only");
    }

    if (models) esp_srmodel_deinit(models);
    return ESP_OK;
}

esp_err_t cap_voice_pipeline_start(void)
{
    s_running = true;
    xTaskCreatePinnedToCore(feed_task,   "vfeed",  FEED_TASK_STACK,
                             NULL, PIPELINE_TASK_PRI, &s_feed_task, 1);
    xTaskCreatePinnedToCore(detect_task, "vdetect", DETECT_TASK_STACK,
                             NULL, PIPELINE_TASK_PRI, &s_detect_task, 1);
    ESP_LOGI(TAG, "Pipeline started, listening for wake word");
    return ESP_OK;
}

esp_err_t cap_voice_pipeline_stop(void)
{
    s_running = false;
    return ESP_OK;
}

cap_voice_state_t cap_voice_pipeline_get_state(void)
{
    return s_state;
}
