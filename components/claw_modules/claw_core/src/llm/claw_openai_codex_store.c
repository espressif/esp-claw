/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "llm/claw_openai_codex_store.h"

#include <stdlib.h>

#include "esp_log.h"
#include "nvs.h"

#define CODEX_NVS_NAMESPACE "codex_auth"
#define CODEX_NVS_KEY_REFRESH "refresh"
#define CODEX_NVS_KEY_ACCOUNT "account"

static const char *TAG = "codex_store";

esp_err_t claw_openai_codex_store_save_refresh_token(const char *token)
{
    if (!token || !token[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CODEX_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, CODEX_NVS_KEY_REFRESH, token);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save refresh token: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t claw_openai_codex_store_load_refresh_token(char **out_token)
{
    if (!out_token) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_token = NULL;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CODEX_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t needed = 0;
    err = nvs_get_str(handle, CODEX_NVS_KEY_REFRESH, NULL, &needed);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    char *token = calloc(1, needed);
    if (!token) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(handle, CODEX_NVS_KEY_REFRESH, token, &needed);
    nvs_close(handle);

    if (err != ESP_OK) {
        free(token);
        return err;
    }

    *out_token = token;
    return ESP_OK;
}

esp_err_t claw_openai_codex_store_save_account_id(const char *account_id)
{
    if (!account_id || !account_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CODEX_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, CODEX_NVS_KEY_ACCOUNT, account_id);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save account id: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t claw_openai_codex_store_load_account_id(char **out_account_id)
{
    if (!out_account_id) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_account_id = NULL;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CODEX_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t needed = 0;
    err = nvs_get_str(handle, CODEX_NVS_KEY_ACCOUNT, NULL, &needed);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    char *account_id = calloc(1, needed);
    if (!account_id) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(handle, CODEX_NVS_KEY_ACCOUNT, account_id, &needed);
    nvs_close(handle);

    if (err != ESP_OK) {
        free(account_id);
        return err;
    }

    *out_account_id = account_id;
    return ESP_OK;
}

esp_err_t claw_openai_codex_store_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CODEX_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}