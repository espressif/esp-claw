/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_demo_settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "rover_demo_settings";
static const char *NS = "rover_demo";

const char *rover_demo_fatfs_base_path = "/fatfs";

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

static void load_defaults(rover_demo_settings_t *s)
{
    copy_str(s->wifi_ssid, sizeof(s->wifi_ssid), CONFIG_ROVER_DEMO_WIFI_SSID);
    copy_str(s->wifi_password, sizeof(s->wifi_password), CONFIG_ROVER_DEMO_WIFI_PASSWORD);
    copy_str(s->llm_api_key, sizeof(s->llm_api_key), CONFIG_ROVER_DEMO_LLM_API_KEY);
    copy_str(s->llm_backend_type, sizeof(s->llm_backend_type), CONFIG_ROVER_DEMO_LLM_BACKEND_TYPE);
    copy_str(s->llm_profile, sizeof(s->llm_profile), CONFIG_ROVER_DEMO_LLM_PROFILE);
    copy_str(s->llm_model, sizeof(s->llm_model), CONFIG_ROVER_DEMO_LLM_MODEL);
    copy_str(s->llm_base_url, sizeof(s->llm_base_url), CONFIG_ROVER_DEMO_LLM_BASE_URL);
    copy_str(s->llm_auth_type, sizeof(s->llm_auth_type), CONFIG_ROVER_DEMO_LLM_AUTH_TYPE);
    copy_str(s->llm_timeout_ms, sizeof(s->llm_timeout_ms), CONFIG_ROVER_DEMO_LLM_TIMEOUT_MS);
    copy_str(s->tg_bot_token, sizeof(s->tg_bot_token), CONFIG_ROVER_DEMO_TG_BOT_TOKEN);
    copy_str(s->time_timezone, sizeof(s->time_timezone), CONFIG_ROVER_DEMO_TIME_TIMEZONE);
}

static void normalize_defaults(rover_demo_settings_t *s)
{
    if (!s->llm_backend_type[0]) {
        copy_str(s->llm_backend_type, sizeof(s->llm_backend_type), CONFIG_ROVER_DEMO_LLM_BACKEND_TYPE);
    }
    if (!s->llm_profile[0]) {
        copy_str(s->llm_profile, sizeof(s->llm_profile), CONFIG_ROVER_DEMO_LLM_PROFILE);
    }
    if (!s->llm_model[0]) {
        copy_str(s->llm_model, sizeof(s->llm_model), CONFIG_ROVER_DEMO_LLM_MODEL);
    }
    if (!s->llm_base_url[0]) {
        copy_str(s->llm_base_url, sizeof(s->llm_base_url), CONFIG_ROVER_DEMO_LLM_BASE_URL);
    }
    if (!s->llm_auth_type[0]) {
        copy_str(s->llm_auth_type, sizeof(s->llm_auth_type), CONFIG_ROVER_DEMO_LLM_AUTH_TYPE);
    }
    if (!s->llm_timeout_ms[0]) {
        copy_str(s->llm_timeout_ms, sizeof(s->llm_timeout_ms), CONFIG_ROVER_DEMO_LLM_TIMEOUT_MS);
    }
    if (!s->time_timezone[0]) {
        copy_str(s->time_timezone, sizeof(s->time_timezone), CONFIG_ROVER_DEMO_TIME_TIMEZONE);
    }
}

esp_err_t rover_demo_settings_init(void)
{
    return ESP_OK;
}

esp_err_t rover_demo_settings_load(rover_demo_settings_t *s)
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
        return ESP_OK;
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

esp_err_t rover_demo_settings_save(const rover_demo_settings_t *s)
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
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        err = nvs_set_str(h, pairs[i].key, pairs[i].val);
        if (err != ESP_OK) {
            break;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t rover_demo_settings_clear(void)
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
