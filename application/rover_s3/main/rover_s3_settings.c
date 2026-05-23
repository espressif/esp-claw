/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_s3_settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#ifndef CONFIG_ROVER_S3_WIFI_SSID
#define CONFIG_ROVER_S3_WIFI_SSID ""
#endif

#ifndef CONFIG_ROVER_S3_WIFI_PASSWORD
#define CONFIG_ROVER_S3_WIFI_PASSWORD ""
#endif

#ifndef CONFIG_ROVER_S3_LLM_API_KEY
#define CONFIG_ROVER_S3_LLM_API_KEY ""
#endif

#ifndef CONFIG_ROVER_S3_LLM_BACKEND_TYPE
#define CONFIG_ROVER_S3_LLM_BACKEND_TYPE "openai_compatible"
#endif

#ifndef CONFIG_ROVER_S3_LLM_PROFILE
#define CONFIG_ROVER_S3_LLM_PROFILE "openai"
#endif

#ifndef CONFIG_ROVER_S3_LLM_MODEL
#define CONFIG_ROVER_S3_LLM_MODEL "openrouter/auto"
#endif

#ifndef CONFIG_ROVER_S3_LLM_BASE_URL
#define CONFIG_ROVER_S3_LLM_BASE_URL "https://openrouter.ai/api/v1"
#endif

#ifndef CONFIG_ROVER_S3_LLM_AUTH_TYPE
#define CONFIG_ROVER_S3_LLM_AUTH_TYPE "bearer"
#endif

#ifndef CONFIG_ROVER_S3_LLM_TIMEOUT_MS
#define CONFIG_ROVER_S3_LLM_TIMEOUT_MS "30000"
#endif

#ifndef CONFIG_ROVER_S3_TG_BOT_TOKEN
#define CONFIG_ROVER_S3_TG_BOT_TOKEN ""
#endif

#ifndef CONFIG_ROVER_S3_TIME_TIMEZONE
#define CONFIG_ROVER_S3_TIME_TIMEZONE "UTC0"
#endif

#ifndef CONFIG_ROVER_S3_WHISPER_API_KEY
#define CONFIG_ROVER_S3_WHISPER_API_KEY ""
#endif

#ifndef CONFIG_ROVER_S3_WHISPER_BASE_URL
#define CONFIG_ROVER_S3_WHISPER_BASE_URL ""
#endif

#ifndef CONFIG_ROVER_S3_TTS_API_KEY
#define CONFIG_ROVER_S3_TTS_API_KEY ""
#endif

#ifndef CONFIG_ROVER_S3_TTS_BASE_URL
#define CONFIG_ROVER_S3_TTS_BASE_URL ""
#endif

#ifndef CONFIG_ROVER_S3_TTS_VOICE
#define CONFIG_ROVER_S3_TTS_VOICE "alloy"
#endif

#ifndef CONFIG_ROVER_S3_WAKE_SENSITIVITY
#define CONFIG_ROVER_S3_WAKE_SENSITIVITY "0.7"
#endif

#ifndef CONFIG_ROVER_S3_MULTINET_THRESHOLD
#define CONFIG_ROVER_S3_MULTINET_THRESHOLD "0.85"
#endif

#ifndef CONFIG_ROVER_S3_VOICE_ENABLED
#define CONFIG_ROVER_S3_VOICE_ENABLED "1"
#endif

static const char *TAG = "rover_s3_settings";
static const char *NS = "rover_s3";

const char *rover_s3_fatfs_base_path = "/fatfs";

typedef struct {
    const char *key;
    char *buf;
    size_t buf_size;
} field_t;

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    strlcpy(dst, src ? src : "", dst_size);
}

static void load_defaults(rover_s3_settings_t *s)
{
    copy_str(s->wifi_ssid, sizeof(s->wifi_ssid), CONFIG_ROVER_S3_WIFI_SSID);
    copy_str(s->wifi_password, sizeof(s->wifi_password), CONFIG_ROVER_S3_WIFI_PASSWORD);
    copy_str(s->llm_api_key, sizeof(s->llm_api_key), CONFIG_ROVER_S3_LLM_API_KEY);
    copy_str(s->llm_backend_type, sizeof(s->llm_backend_type), CONFIG_ROVER_S3_LLM_BACKEND_TYPE);
    copy_str(s->llm_profile, sizeof(s->llm_profile), CONFIG_ROVER_S3_LLM_PROFILE);
    copy_str(s->llm_model, sizeof(s->llm_model), CONFIG_ROVER_S3_LLM_MODEL);
    copy_str(s->llm_base_url, sizeof(s->llm_base_url), CONFIG_ROVER_S3_LLM_BASE_URL);
    copy_str(s->llm_auth_type, sizeof(s->llm_auth_type), CONFIG_ROVER_S3_LLM_AUTH_TYPE);
    copy_str(s->llm_timeout_ms, sizeof(s->llm_timeout_ms), CONFIG_ROVER_S3_LLM_TIMEOUT_MS);
    copy_str(s->tg_bot_token, sizeof(s->tg_bot_token), CONFIG_ROVER_S3_TG_BOT_TOKEN);
    copy_str(s->time_timezone, sizeof(s->time_timezone), CONFIG_ROVER_S3_TIME_TIMEZONE);
    copy_str(s->whisper_api_key, sizeof(s->whisper_api_key), CONFIG_ROVER_S3_WHISPER_API_KEY);
    copy_str(s->whisper_base_url, sizeof(s->whisper_base_url), CONFIG_ROVER_S3_WHISPER_BASE_URL);
    copy_str(s->tts_api_key, sizeof(s->tts_api_key), CONFIG_ROVER_S3_TTS_API_KEY);
    copy_str(s->tts_base_url, sizeof(s->tts_base_url), CONFIG_ROVER_S3_TTS_BASE_URL);
    copy_str(s->tts_voice, sizeof(s->tts_voice), CONFIG_ROVER_S3_TTS_VOICE);
    copy_str(s->wake_sensitivity, sizeof(s->wake_sensitivity), CONFIG_ROVER_S3_WAKE_SENSITIVITY);
    copy_str(s->multinet_threshold, sizeof(s->multinet_threshold), CONFIG_ROVER_S3_MULTINET_THRESHOLD);
    copy_str(s->voice_enabled, sizeof(s->voice_enabled), CONFIG_ROVER_S3_VOICE_ENABLED);
}

static void normalize_defaults(rover_s3_settings_t *s)
{
    if (!s->llm_backend_type[0]) {
        copy_str(s->llm_backend_type, sizeof(s->llm_backend_type), CONFIG_ROVER_S3_LLM_BACKEND_TYPE);
    }
    if (!s->llm_profile[0]) {
        copy_str(s->llm_profile, sizeof(s->llm_profile), CONFIG_ROVER_S3_LLM_PROFILE);
    }
    if (!s->llm_model[0]) {
        copy_str(s->llm_model, sizeof(s->llm_model), CONFIG_ROVER_S3_LLM_MODEL);
    }
    if (!s->llm_base_url[0]) {
        copy_str(s->llm_base_url, sizeof(s->llm_base_url), CONFIG_ROVER_S3_LLM_BASE_URL);
    }
    if (!s->llm_auth_type[0]) {
        copy_str(s->llm_auth_type, sizeof(s->llm_auth_type), CONFIG_ROVER_S3_LLM_AUTH_TYPE);
    }
    if (!s->llm_timeout_ms[0]) {
        copy_str(s->llm_timeout_ms, sizeof(s->llm_timeout_ms), CONFIG_ROVER_S3_LLM_TIMEOUT_MS);
    }
    if (!s->time_timezone[0]) {
        copy_str(s->time_timezone, sizeof(s->time_timezone), CONFIG_ROVER_S3_TIME_TIMEZONE);
    }
    if (!s->tts_voice[0]) {
        copy_str(s->tts_voice, sizeof(s->tts_voice), CONFIG_ROVER_S3_TTS_VOICE);
    }
    if (!s->wake_sensitivity[0]) {
        copy_str(s->wake_sensitivity, sizeof(s->wake_sensitivity), CONFIG_ROVER_S3_WAKE_SENSITIVITY);
    }
    if (!s->multinet_threshold[0]) {
        copy_str(s->multinet_threshold, sizeof(s->multinet_threshold), CONFIG_ROVER_S3_MULTINET_THRESHOLD);
    }
    if (!s->voice_enabled[0]) {
        copy_str(s->voice_enabled, sizeof(s->voice_enabled), CONFIG_ROVER_S3_VOICE_ENABLED);
    }
}

esp_err_t rover_s3_settings_init(void)
{
    return ESP_OK;
}

esp_err_t rover_s3_settings_load(rover_s3_settings_t *s)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(s, 0, sizeof(*s));
    load_defaults(s);

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        normalize_defaults(s);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s, using defaults", esp_err_to_name(err));
        normalize_defaults(s);
        return err;
    }

    field_t fields[] = {
        {"wifi_ssid", s->wifi_ssid, sizeof(s->wifi_ssid)},
        {"wifi_password", s->wifi_password, sizeof(s->wifi_password)},
        {"llm_api_key", s->llm_api_key, sizeof(s->llm_api_key)},
        {"llm_backend", s->llm_backend_type, sizeof(s->llm_backend_type)},
        {"llm_profile", s->llm_profile, sizeof(s->llm_profile)},
        {"llm_model", s->llm_model, sizeof(s->llm_model)},
        {"llm_base_url", s->llm_base_url, sizeof(s->llm_base_url)},
        {"llm_auth", s->llm_auth_type, sizeof(s->llm_auth_type)},
        {"llm_timeout_ms", s->llm_timeout_ms, sizeof(s->llm_timeout_ms)},
        {"tg_bot_token", s->tg_bot_token, sizeof(s->tg_bot_token)},
        {"time_timezone", s->time_timezone, sizeof(s->time_timezone)},
        {"whisper_api_key", s->whisper_api_key, sizeof(s->whisper_api_key)},
        {"whisper_base", s->whisper_base_url, sizeof(s->whisper_base_url)},
        {"tts_api_key", s->tts_api_key, sizeof(s->tts_api_key)},
        {"tts_base_url", s->tts_base_url, sizeof(s->tts_base_url)},
        {"tts_voice", s->tts_voice, sizeof(s->tts_voice)},
        {"wake_sens", s->wake_sensitivity, sizeof(s->wake_sensitivity)},
        {"mn_thresh", s->multinet_threshold, sizeof(s->multinet_threshold)},
        {"voice_enabled", s->voice_enabled, sizeof(s->voice_enabled)},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        size_t len = fields[i].buf_size;
        esp_err_t e = nvs_get_str(h, fields[i].key, fields[i].buf, &len);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_get_str(%s) failed: %s", fields[i].key, esp_err_to_name(e));
        }
    }
    nvs_close(h);
    normalize_defaults(s);
    return ESP_OK;
}

esp_err_t rover_s3_settings_save(const rover_s3_settings_t *s)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    typedef struct {
        const char *key;
        const char *val;
    } pair_t;
    pair_t pairs[] = {
        {"wifi_ssid", s->wifi_ssid},
        {"wifi_password", s->wifi_password},
        {"llm_api_key", s->llm_api_key},
        {"llm_backend", s->llm_backend_type},
        {"llm_profile", s->llm_profile},
        {"llm_model", s->llm_model},
        {"llm_base_url", s->llm_base_url},
        {"llm_auth", s->llm_auth_type},
        {"llm_timeout_ms", s->llm_timeout_ms},
        {"tg_bot_token", s->tg_bot_token},
        {"time_timezone", s->time_timezone},
        {"whisper_api_key", s->whisper_api_key},
        {"whisper_base", s->whisper_base_url},
        {"tts_api_key", s->tts_api_key},
        {"tts_base_url", s->tts_base_url},
        {"tts_voice", s->tts_voice},
        {"wake_sens", s->wake_sensitivity},
        {"mn_thresh", s->multinet_threshold},
        {"voice_enabled", s->voice_enabled},
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        err = nvs_set_str(h, pairs[i].key, pairs[i].val);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs_set_str(%s) failed: %s", pairs[i].key, esp_err_to_name(err));
            break;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t rover_s3_settings_clear(void)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
