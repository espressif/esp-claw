/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "claw_cap.h"
#include "claw_task.h"
#include "cap_time.h"


static const char *TAG = "cap_time";

#define CAP_TIME_SNTP_PRIMARY_SERVER "pool.ntp.org"
#define CAP_TIME_SNTP_SECONDARY_SERVER "time.windows.com"
#define CAP_TIME_SNTP_WAIT_MS 3000
#define CAP_TIME_SNTP_RETRY_COUNT 15
#define CAP_TIME_MIN_VALID_EPOCH 1704067200
#define CAP_TIME_DEFAULT_DISCONNECTED_RETRY_MS 5000
#define CAP_TIME_DEFAULT_SYNC_RETRY_MS        30000

static SemaphoreHandle_t s_time_mutex = NULL;
static struct {
    TaskHandle_t task_handle;
    cap_time_sync_service_config_t config;
    bool running;
} s_time_service = {0};

static esp_err_t cap_time_ensure_mutex(void)
{
    if (!s_time_mutex) {
        s_time_mutex = xSemaphoreCreateMutex();
    }
    return s_time_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t cap_time_read_timezone(char *timezone, size_t timezone_size)
{
    const char *configured_tz = NULL;

    if (!timezone || timezone_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    configured_tz = getenv("TZ");
    if (configured_tz && configured_tz[0]) {
        strlcpy(timezone, configured_tz, timezone_size);
        return ESP_OK;
    }

    return ESP_ERR_INVALID_STATE;
}

static void cap_time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "SNTP time synchronization event received");
}

static esp_err_t cap_time_build_prompt_block(char *output, size_t output_size)
{
    time_t now = 0;
    struct tm local_tm = {0};
    struct timeval tv = {0};
    char formatted_time[64] = {0};
    int written;
    bool time_valid;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    time(&now);
    gettimeofday(&tv, NULL);
    time_valid = cap_time_is_valid();
    if (time_valid) {
        if (!localtime_r(&now, &local_tm)) {
            return ESP_FAIL;
        }
        if (strftime(formatted_time, sizeof(formatted_time), "%Y-%m-%d %H:%M:%S %Z (%A)", &local_tm) == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        strlcpy(formatted_time, "(invalid)", sizeof(formatted_time));
    }

    written = snprintf(output, output_size,
                       "- current_local_time: %s\n"
                       "- unix_timestamp: %lld\n",
                       formatted_time, (long long)tv.tv_sec);
    if (written < 0 || (size_t)written >= output_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cap_time_format_current_time(char *output, size_t output_size)
{
    time_t now = 0;
    struct tm local_tm = {0};

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    time(&now);
    if (now < CAP_TIME_MIN_VALID_EPOCH) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!localtime_r(&now, &local_tm)) {
        return ESP_FAIL;
    }
    if (strftime(output, output_size, "%Y-%m-%d %H:%M:%S %Z (%A)", &local_tm) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cap_time_sync_with_sntp(char *output, size_t output_size)
{
    esp_err_t err = ESP_OK;
    esp_err_t wait_err = ESP_OK;
    int retry = 0;
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2,
        ESP_SNTP_SERVER_LIST(CAP_TIME_SNTP_PRIMARY_SERVER, CAP_TIME_SNTP_SECONDARY_SERVER)
    );
#else
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CAP_TIME_SNTP_PRIMARY_SERVER);
#endif

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    config.sync_cb = cap_time_sync_notification_cb;
    err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        return err;
    }

    while ((wait_err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(CAP_TIME_SNTP_WAIT_MS))) == ESP_ERR_TIMEOUT &&
           ++retry < CAP_TIME_SNTP_RETRY_COUNT) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, CAP_TIME_SNTP_RETRY_COUNT);
    }

    if (wait_err != ESP_OK) {
        err = wait_err;
        goto done;
    }

    err = cap_time_format_current_time(output, output_size);

done:
    esp_netif_sntp_deinit();
    if (err == ESP_OK) {
        return ESP_OK;
    }

    return err;
}

static esp_err_t cap_time_context_collect(const claw_core_request_t *request, claw_core_context_t *out_context, void *user_ctx)
{
    char *content = NULL;
    esp_err_t err;
    const size_t content_size = 384;

    (void)request;
    (void)user_ctx;

    if (!out_context) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_context, 0, sizeof(*out_context));

    content = calloc(1, content_size);
    if (!content) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_time_ensure_mutex();
    if (err != ESP_OK) {
        free(content);
        ESP_LOGE(TAG, "Failed to create time mutex");
        return err;
    }
    xSemaphoreTake(s_time_mutex, portMAX_DELAY);
    err = cap_time_build_prompt_block(content, content_size);
    xSemaphoreGive(s_time_mutex);
    if (err != ESP_OK) {
        free(content);
        return err;
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = content;
    return ESP_OK;
}

esp_err_t cap_time_get_current(char *output, size_t output_size)
{
    esp_err_t err;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(cap_time_ensure_mutex(), TAG, "Failed to create time mutex");
    xSemaphoreTake(s_time_mutex, portMAX_DELAY);
    err = cap_time_is_valid() ? cap_time_format_current_time(output, output_size) : cap_time_sync_with_sntp(output, output_size);
    xSemaphoreGive(s_time_mutex);
    return err;
}

esp_err_t cap_time_sync_now(char *output, size_t output_size)
{
    esp_err_t err;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(cap_time_ensure_mutex(), TAG, "Failed to create time mutex");
    xSemaphoreTake(s_time_mutex, portMAX_DELAY);
    err = cap_time_sync_with_sntp(output, output_size);
    xSemaphoreGive(s_time_mutex);
    return err;
}

esp_err_t cap_time_get_timezone(char *timezone, size_t timezone_size)
{
    esp_err_t err;

    if (!timezone || timezone_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(cap_time_ensure_mutex(), TAG, "Failed to create time mutex");
    xSemaphoreTake(s_time_mutex, portMAX_DELAY);
    err = cap_time_read_timezone(timezone, timezone_size);
    xSemaphoreGive(s_time_mutex);
    return err;
}

bool cap_time_is_valid(void)
{
    time_t now = time(NULL);

    return now >= CAP_TIME_MIN_VALID_EPOCH;
}

static bool cap_time_network_ready(void)
{
    if (!s_time_service.config.network_ready) {
        return true;
    }

    return s_time_service.config.network_ready(s_time_service.config.network_ready_ctx);
}

static void cap_time_notify_sync_success(bool had_valid_time)
{
    if (s_time_service.config.on_sync_success) {
        s_time_service.config.on_sync_success(had_valid_time, s_time_service.config.on_sync_success_ctx);
    }
}

static void cap_time_sync_service_task(void *arg)
{
    char output[256];

    (void)arg;

    while (s_time_service.running) {
        bool time_valid;
        uint32_t delay_ms;

        if (!cap_time_network_ready()) {
            delay_ms = s_time_service.config.disconnected_retry_ms;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue;
        }

        time_valid = cap_time_is_valid();
        if (!time_valid) {
            bool had_valid_time = false;
            esp_err_t err;

            output[0] = '\0';
            err = cap_time_sync_now(output, sizeof(output));
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Time sync succeeded: %s", output);
                cap_time_notify_sync_success(had_valid_time);
                break;
            }

            ESP_LOGW(TAG, "Time sync failed: %s", esp_err_to_name(err));
            delay_ms = s_time_service.config.sync_retry_ms;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue;
        }

        cap_time_notify_sync_success(false);
        break;
    }

    s_time_service.running = false;
    s_time_service.task_handle = NULL;
    claw_task_delete(NULL);
}

static esp_err_t cap_time_execute(const char *input_json, const claw_cap_call_context_t *ctx, char *output, size_t output_size)
{
    esp_err_t err;

    (void)input_json;
    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_time_get_current(output, output_size);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to get time (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", output);
        return err;
    }

    ESP_LOGI(TAG, "Current time: %s", output);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_time_descriptors[] = {
    {
        .id = "get_current_time",
        .name = "get_current_time",
        .family = "system",
        .description = "Return formatted current local time",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_time_execute,
    },
};

static const claw_cap_group_t s_time_group = {
    .group_id = "cap_time",
    .descriptors = s_time_descriptors,
    .descriptor_count = sizeof(s_time_descriptors) / sizeof(s_time_descriptors[0]),
};

esp_err_t cap_time_register_group(void)
{
    if (claw_cap_group_exists(s_time_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_time_group);
}

esp_err_t cap_time_sync_service_start(const cap_time_sync_service_config_t *config)
{
    BaseType_t ok;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_time_service.task_handle || s_time_service.running) {
        return ESP_OK;
    }

    memset(&s_time_service.config, 0, sizeof(s_time_service.config));
    s_time_service.config = *config;
    if (s_time_service.config.disconnected_retry_ms == 0) {
        s_time_service.config.disconnected_retry_ms = CAP_TIME_DEFAULT_DISCONNECTED_RETRY_MS;
    }
    if (s_time_service.config.sync_retry_ms == 0) {
        s_time_service.config.sync_retry_ms = CAP_TIME_DEFAULT_SYNC_RETRY_MS;
    }

    if (cap_time_is_valid()) {
        cap_time_notify_sync_success(false);
        return ESP_OK;
    }

    s_time_service.running = true;

    ok = claw_task_create(&(claw_task_config_t){
                              .name = "cap_time_sync",
                              .stack_size = 4096,
                              .priority = 5,
                              .core_id = tskNO_AFFINITY,
                              .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                          }, cap_time_sync_service_task, NULL, &s_time_service.task_handle);
    if (ok != pdPASS || !s_time_service.task_handle) {
        s_time_service.running = false;
        s_time_service.task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t cap_time_sync_service_stop(void)
{
    TaskHandle_t task = s_time_service.task_handle;

    s_time_service.running = false;
    if (!task) {
        return ESP_OK;
    }

    while (s_time_service.task_handle == task) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

const claw_core_context_provider_t cap_time_context_provider = {
    .name = "Time Context",
    .collect = cap_time_context_collect,
    .user_ctx = NULL,
};
