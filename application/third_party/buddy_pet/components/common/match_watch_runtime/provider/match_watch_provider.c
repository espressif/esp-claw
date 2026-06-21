/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "match_watch_runtime.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "match_watch_provider";
static const char *MATCH_WATCH_PROVIDER_EXCLUSIVE = "match_watch_provider";
static const uint32_t MATCH_WATCH_PROVIDER_TIMEOUT_MS = 0;
static const char *MATCH_WATCH_PROVIDER_DEFAULT_NAME = "match_watch_provider";
static const int MATCH_WATCH_PROVIDER_STARTING_GRACE_S = 10;

typedef struct {
    SemaphoreHandle_t lock;
    char name[32];
    char path[192];
    char args_summary[192];
    char job_id[16];
    time_t started_at;
    time_t last_push_at;
    time_t last_error_at;
    char last_error[128];
} match_watch_provider_state_t;

typedef struct {
    match_watch_runtime_provider_run_cb_t run_cb;
    match_watch_runtime_provider_stop_cb_t stop_cb;
} match_watch_provider_backend_t;

static match_watch_provider_state_t s_provider;
static match_watch_provider_backend_t s_backend;

static esp_err_t match_watch_provider_ensure_lock(void)
{
    if (s_provider.lock == NULL) {
        s_provider.lock = xSemaphoreCreateMutex();
    }
    return s_provider.lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void match_watch_provider_copy_json_summary(const char *json, char *out, size_t out_size)
{
    size_t len;

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (json == NULL || json[0] == '\0') {
        return;
    }
    len = strlen(json);
    if (len < out_size) {
        strlcpy(out, json, out_size);
        return;
    }
    if (out_size <= 4) {
        strlcpy(out, "...", out_size);
        return;
    }
    memcpy(out, json, out_size - 4);
    memcpy(out + out_size - 4, "...", 4);
}

static bool match_watch_provider_args_match_summary(const char *args_json, const char *summary)
{
    char expected[sizeof(s_provider.args_summary)] = {0};

    match_watch_provider_copy_json_summary(args_json, expected, sizeof(expected));
    return strcmp(expected, summary != NULL ? summary : "") == 0;
}

static bool match_watch_provider_request_already_active(const char *provider_name,
                                                        const char *path,
                                                        const char *args_json)
{
    bool active = false;
    time_t now = time(NULL);

    if (match_watch_provider_ensure_lock() != ESP_OK) {
        return false;
    }

    xSemaphoreTake(s_provider.lock, portMAX_DELAY);
    active = (s_provider.job_id[0] != '\0' ||
              (s_provider.started_at > 0 && (int)(now - s_provider.started_at) <= MATCH_WATCH_PROVIDER_STARTING_GRACE_S)) &&
             s_provider.last_error[0] == '\0' &&
             strcmp(s_provider.name, provider_name != NULL ? provider_name : "") == 0 &&
             strcmp(s_provider.path, path != NULL ? path : "") == 0 &&
             match_watch_provider_args_match_summary(args_json, s_provider.args_summary);
    xSemaphoreGive(s_provider.lock);
    return active;
}

static bool match_watch_provider_extract_lua_job_id(const char *text, char *out, size_t out_size)
{
    const char *p;
    const char *end;
    size_t len;

    if (text == NULL || out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    p = strstr(text, "Lua job ");
    if (p == NULL) {
        p = strstr(text, "Started Lua job ");
        if (p == NULL) {
            return false;
        }
        p += strlen("Started Lua job ");
    } else {
        p += strlen("Lua job ");
    }
    end = p;
    while (*end != '\0' && *end != ' ' && *end != '(' && *end != '\n' && *end != '\r') {
        end++;
    }
    len = (size_t)(end - p);
    if (len == 0 || len >= out_size) {
        return false;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

void match_watch_runtime_provider_record_error(const char *message)
{
    if (match_watch_provider_ensure_lock() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_provider.lock, portMAX_DELAY);
    s_provider.last_error_at = time(NULL);
    if (message != NULL) {
        strlcpy(s_provider.last_error, message, sizeof(s_provider.last_error));
    } else {
        s_provider.last_error[0] = '\0';
    }
    xSemaphoreGive(s_provider.lock);
}

void match_watch_runtime_provider_record_push_success(void *user_data)
{
    (void)user_data;

    if (match_watch_provider_ensure_lock() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_provider.lock, portMAX_DELAY);
    s_provider.last_push_at = time(NULL);
    s_provider.last_error[0] = '\0';
    s_provider.last_error_at = 0;
    xSemaphoreGive(s_provider.lock);
}

static void match_watch_provider_record_start_attempt(const char *provider_name,
                                                      const char *path,
                                                      const char *args_json)
{
    if (match_watch_provider_ensure_lock() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_provider.lock, portMAX_DELAY);
    strlcpy(s_provider.name,
            provider_name != NULL ? provider_name : MATCH_WATCH_PROVIDER_DEFAULT_NAME,
            sizeof(s_provider.name));
    strlcpy(s_provider.path, path != NULL ? path : "", sizeof(s_provider.path));
    match_watch_provider_copy_json_summary(args_json, s_provider.args_summary, sizeof(s_provider.args_summary));
    s_provider.job_id[0] = '\0';
    s_provider.started_at = time(NULL);
    s_provider.last_push_at = 0;
    s_provider.last_error[0] = '\0';
    s_provider.last_error_at = 0;
    xSemaphoreGive(s_provider.lock);
}

static void match_watch_provider_write_status(char *output,
                                              size_t output_size,
                                              bool ok,
                                              const char *status)
{
    time_t now = time(NULL);
    char provider_name[sizeof(s_provider.name)] = {0};
    char provider_path[sizeof(s_provider.path)] = {0};
    char provider_args_summary[sizeof(s_provider.args_summary)] = {0};
    char provider_job_id[sizeof(s_provider.job_id)] = {0};
    char provider_last_error[sizeof(s_provider.last_error)] = {0};
    time_t provider_started_at = 0;
    time_t provider_last_push_at = 0;
    time_t provider_last_error_at = 0;
    bool provider_running = false;
    cJSON *root = NULL;
    char *json = NULL;

    if (output == NULL || output_size == 0) {
        return;
    }
    if (match_watch_provider_ensure_lock() != ESP_OK) {
        snprintf(output, output_size, "{\"ok\":false,\"status\":\"no_mem\"}");
        return;
    }

    xSemaphoreTake(s_provider.lock, portMAX_DELAY);
    provider_running = s_provider.job_id[0] != '\0';
    strlcpy(provider_name, s_provider.name, sizeof(provider_name));
    strlcpy(provider_path, s_provider.path, sizeof(provider_path));
    strlcpy(provider_args_summary, s_provider.args_summary, sizeof(provider_args_summary));
    strlcpy(provider_job_id, s_provider.job_id, sizeof(provider_job_id));
    strlcpy(provider_last_error, s_provider.last_error, sizeof(provider_last_error));
    provider_started_at = s_provider.started_at;
    provider_last_push_at = s_provider.last_push_at;
    provider_last_error_at = s_provider.last_error_at;
    xSemaphoreGive(s_provider.lock);

    root = cJSON_CreateObject();
    if (root == NULL) {
        snprintf(output, output_size, "{\"ok\":false,\"status\":\"no_mem\"}");
        return;
    }
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "status", status != NULL ? status : "unknown");
    cJSON_AddBoolToObject(root, "provider_running", provider_running);
    cJSON_AddStringToObject(root, "provider", provider_name);
    cJSON_AddStringToObject(root, "path", provider_path);
    cJSON_AddStringToObject(root, "job_id", provider_job_id);
    cJSON_AddStringToObject(root, "exclusive", MATCH_WATCH_PROVIDER_EXCLUSIVE);
    cJSON_AddNumberToObject(root, "runtime_s",
                            provider_started_at > 0 ? (int)(now - provider_started_at) : 0);
    cJSON_AddNumberToObject(root, "last_push_age_s",
                            provider_last_push_at > 0 ? (int)(now - provider_last_push_at) : -1);
    cJSON_AddNumberToObject(root, "last_error_age_s",
                            provider_last_error_at > 0 ? (int)(now - provider_last_error_at) : -1);
    cJSON_AddStringToObject(root, "last_error", provider_last_error);
    cJSON_AddStringToObject(root, "args", provider_args_summary);
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        snprintf(output, output_size, "{\"ok\":false,\"status\":\"no_mem\"}");
        return;
    }
    strlcpy(output, json, output_size);
    free(json);
}

esp_err_t match_watch_runtime_provider_set_backend(match_watch_runtime_provider_run_cb_t run_cb,
                                                   match_watch_runtime_provider_stop_cb_t stop_cb)
{
    if (run_cb == NULL || stop_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_backend.run_cb = run_cb;
    s_backend.stop_cb = stop_cb;
    return ESP_OK;
}

esp_err_t match_watch_runtime_provider_start(const char *path,
                                             const char *args_json,
                                             const char *name,
                                             char *output,
                                             size_t output_size)
{
    char lua_output[512] = {0};
    char job_id[16] = {0};
    const char *provider_name = (name != NULL && name[0] != '\0') ? name : MATCH_WATCH_PROVIDER_DEFAULT_NAME;
    bool ended_early = false;
    esp_err_t ret;

    if (path == NULL || path[0] == '\0') {
        match_watch_runtime_provider_record_error("provider path is required");
        match_watch_provider_write_status(output, output_size, false, "missing_path");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_backend.run_cb == NULL) {
        match_watch_runtime_provider_record_error("provider backend is unavailable");
        match_watch_provider_write_status(output, output_size, false, "backend_unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    if (match_watch_provider_request_already_active(provider_name, path, args_json)) {
        match_watch_provider_write_status(output, output_size, true, "provider_already_active");
        ESP_LOGI(TAG, "match provider already active: name=%s", provider_name);
        return ESP_OK;
    }

    match_watch_provider_record_start_attempt(provider_name, path, args_json);
    ret = s_backend.run_cb(path,
                           args_json,
                           MATCH_WATCH_PROVIDER_TIMEOUT_MS,
                           provider_name,
                           MATCH_WATCH_PROVIDER_EXCLUSIVE,
                           true,
                           lua_output,
                           sizeof(lua_output));
    if (ret == ESP_OK && strstr(lua_output, "ended early") != NULL) {
        ended_early = true;
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        (void)match_watch_provider_extract_lua_job_id(lua_output, job_id, sizeof(job_id));
        if (match_watch_provider_ensure_lock() == ESP_OK) {
            xSemaphoreTake(s_provider.lock, portMAX_DELAY);
            strlcpy(s_provider.name, provider_name, sizeof(s_provider.name));
            strlcpy(s_provider.path, path, sizeof(s_provider.path));
            match_watch_provider_copy_json_summary(args_json, s_provider.args_summary, sizeof(s_provider.args_summary));
            if (job_id[0] != '\0') {
                strlcpy(s_provider.job_id, job_id, sizeof(s_provider.job_id));
            } else {
                s_provider.job_id[0] = '\0';
            }
            s_provider.started_at = time(NULL);
            s_provider.last_error[0] = '\0';
            s_provider.last_error_at = 0;
            xSemaphoreGive(s_provider.lock);
        }
        match_watch_provider_write_status(output, output_size, true, "provider_started");
    } else {
        match_watch_runtime_provider_record_error(lua_output[0] ? lua_output : esp_err_to_name(ret));
        ESP_LOGW(TAG, "match provider start failed: name=%s err=%s detail=%s",
                 provider_name,
                 esp_err_to_name(ret),
                 lua_output[0] != '\0' ? lua_output : "(none)");
        match_watch_provider_write_status(output,
                                          output_size,
                                          false,
                                          ended_early ? "provider_ended_early" : "provider_start_failed");
    }
    return ret;
}

esp_err_t match_watch_runtime_provider_stop(uint32_t wait_ms, char *output, size_t output_size)
{
    char lua_output[256] = {0};
    esp_err_t ret;

    if (wait_ms == 0) {
        wait_ms = MATCH_WATCH_RUNTIME_PROVIDER_STOP_WAIT_MS;
    }
    if (s_backend.stop_cb == NULL) {
        match_watch_runtime_provider_record_error("provider backend is unavailable");
        match_watch_provider_write_status(output, output_size, false, "backend_unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    ret = s_backend.stop_cb(MATCH_WATCH_PROVIDER_EXCLUSIVE, wait_ms, lua_output, sizeof(lua_output));
    if (ret == ESP_OK && match_watch_provider_ensure_lock() == ESP_OK) {
        xSemaphoreTake(s_provider.lock, portMAX_DELAY);
        s_provider.job_id[0] = '\0';
        s_provider.started_at = 0;
        xSemaphoreGive(s_provider.lock);
        match_watch_provider_write_status(output, output_size, true, "provider_stopped");
    } else {
        match_watch_runtime_provider_record_error(lua_output[0] ? lua_output : esp_err_to_name(ret));
        match_watch_provider_write_status(output, output_size, false, "provider_stop_failed");
    }
    return ret;
}

esp_err_t match_watch_runtime_provider_status(char *output, size_t output_size)
{
    match_watch_provider_write_status(output, output_size, true, "provider_status");
    return ESP_OK;
}
