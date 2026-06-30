/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "settings_store.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings_store";

static char s_namespace[16];
static bool s_initialized;

static esp_err_t settings_store_open(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    if (!s_initialized || s_namespace[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    return nvs_open(s_namespace, mode, handle);
}

esp_err_t settings_store_init(const settings_store_config_t *config)
{
    if (!config || !config->namespace_name || config->namespace_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlcpy(s_namespace, config->namespace_name, sizeof(s_namespace)) >= sizeof(s_namespace)) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_initialized = true;

    nvs_handle_t handle;
    esp_err_t err = settings_store_open(NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_close(handle);
        return ESP_OK;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = settings_store_open(NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            nvs_close(handle);
        }
    }

    return err;
}

esp_err_t settings_store_get_string(const char *key,
                                    char *buf,
                                    size_t buf_size,
                                    const char *default_value)
{
    if (!key || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    buf[0] = '\0';

    nvs_handle_t handle;
    esp_err_t err = settings_store_open(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (default_value) {
            strlcpy(buf, default_value, buf_size);
        }
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = buf_size;
    err = nvs_get_str(handle, key, buf, &required_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (default_value) {
            strlcpy(buf, default_value, buf_size);
        }
        return ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_str(%s) failed: %s", key, esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t settings_store_has_key(const char *key, bool *exists)
{
    if (!key || !exists) {
        return ESP_ERR_INVALID_ARG;
    }

    *exists = false;

    nvs_handle_t handle;
    esp_err_t err = settings_store_open(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = 0;
    err = nvs_get_str(handle, key, NULL, &required_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_str(%s) failed: %s", key, esp_err_to_name(err));
        return err;
    }

    *exists = true;
    return ESP_OK;
}

esp_err_t settings_store_set_string(const char *key, const char *value)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = settings_store_open(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, key, value ? value : "");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) failed: %s", key, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t settings_store_erase_key(const char *key)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = settings_store_open(NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_key(%s) failed: %s", key, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t settings_store_commit(void)
{
    return ESP_OK;
}
