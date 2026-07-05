/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_system.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_task.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static const char *TAG = "cap_system";

#define CAP_SYSTEM_RESTART_DEFAULT_DELAY_MS 500

#ifdef CONFIG_CLAW_CAP_SYSTEM_DEBUG_LOGS
#define CAP_SYSTEM_DEBUG_LOG(label, text) ESP_LOGI(TAG, "%s: %s", label, text)
#else
#define CAP_SYSTEM_DEBUG_LOG(label, text) do { (void)(label); (void)(text); } while (0)
#endif

typedef struct {
    uint32_t delay_ms;
} cap_system_restart_task_args_t;

static const char *cap_system_wifi_auth_mode_to_str(wifi_auth_mode_t authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN:
            return "open";
        case WIFI_AUTH_WEP:
            return "wep";
        case WIFI_AUTH_WPA_PSK:
            return "wpa_psk";
        case WIFI_AUTH_WPA2_PSK:
            return "wpa2_psk";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "wpa_wpa2_psk";
        case WIFI_AUTH_WPA2_ENTERPRISE:
            return "wpa2_enterprise";
        case WIFI_AUTH_WPA3_PSK:
            return "wpa3_psk";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "wpa2_wpa3_psk";
        case WIFI_AUTH_WAPI_PSK:
            return "wapi_psk";
        default:
            return "unknown";
    }
}

static esp_err_t cap_system_render_json(cJSON *root, char *output, size_t output_size)
{
    char *rendered = NULL;

    if (!root || !output || output_size == 0) {
        ESP_LOGE(TAG, "render json: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }

    rendered = cJSON_PrintUnformatted(root);
    if (!rendered) {
        ESP_LOGE(TAG, "render json: no mem");
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", rendered);
    free(rendered);
    return ESP_OK;
}

static cJSON *cap_system_build_memory_json(void)
{
    cJSON *root = cJSON_CreateObject();
    size_t psram_total = 0;

    if (!root) {
        ESP_LOGE(TAG, "memory json: create failed");
        return NULL;
    }

    cJSON_AddNumberToObject(root, "internal_free", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "internal_total", (double)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "internal_min_free", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "internal_largest_free_block", (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min_free", (double)esp_get_minimum_free_heap_size());

    psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    cJSON_AddBoolToObject(root, "psram_available", psram_total > 0);
    if (psram_total > 0) {
        cJSON_AddNumberToObject(root, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(root, "psram_total", (double)psram_total);
        cJSON_AddNumberToObject(root,
                                "psram_largest_free_block",
                                (double)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }

    return root;
}

static esp_err_t cap_system_get_sta_ip_info(esp_netif_ip_info_t *ip_info)
{
    esp_netif_t *netif = NULL;

    if (!ip_info) {
        ESP_LOGE(TAG, "sta ip: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        ESP_LOGE(TAG, "sta ip: netif not found");
        return ESP_ERR_NOT_FOUND;
    }

    return esp_netif_get_ip_info(netif, ip_info);
}

static cJSON *cap_system_build_ip_json(void)
{
    cJSON *root = cJSON_CreateObject();
    esp_netif_ip_info_t ip_info = {0};
    esp_err_t err;
    char ip_buf[16];
    char netmask_buf[16];
    char gateway_buf[16];

    if (!root) {
        ESP_LOGE(TAG, "ip json: create failed");
        return NULL;
    }

    err = cap_system_get_sta_ip_info(&ip_info);
    if (err != ESP_OK || ip_info.ip.addr == 0) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ip info unavailable: %s", esp_err_to_name(err));
        }
        cJSON_AddBoolToObject(root, "connected", false);
        cJSON_AddNullToObject(root, "ip");
        cJSON_AddNullToObject(root, "netmask");
        cJSON_AddNullToObject(root, "gateway");
        return root;
    }

    snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip_info.ip));
    snprintf(netmask_buf, sizeof(netmask_buf), IPSTR, IP2STR(&ip_info.netmask));
    snprintf(gateway_buf, sizeof(gateway_buf), IPSTR, IP2STR(&ip_info.gw));

    cJSON_AddBoolToObject(root, "connected", true);
    cJSON_AddStringToObject(root, "ip", ip_buf);
    cJSON_AddStringToObject(root, "netmask", netmask_buf);
    cJSON_AddStringToObject(root, "gateway", gateway_buf);
    return root;
}

static cJSON *cap_system_build_wifi_json(void)
{
    cJSON *root = cJSON_CreateObject();
    wifi_ap_record_t ap_info = {0};
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_err_t err;
    char bssid_buf[18];

    if (!root) {
        ESP_LOGE(TAG, "wifi json: create failed");
        return NULL;
    }

    err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi ap info failed: %s", esp_err_to_name(err));
        cJSON_AddBoolToObject(root, "connected", false);
        cJSON_AddStringToObject(root, "status", "disconnected");
        return root;
    }

    snprintf(bssid_buf,
             sizeof(bssid_buf),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             ap_info.bssid[0],
             ap_info.bssid[1],
             ap_info.bssid[2],
             ap_info.bssid[3],
             ap_info.bssid[4],
             ap_info.bssid[5]);

    cJSON_AddBoolToObject(root, "connected", true);
    cJSON_AddStringToObject(root, "status", "connected");
    cJSON_AddStringToObject(root, "ssid", (const char *)ap_info.ssid);
    cJSON_AddStringToObject(root, "bssid", bssid_buf);
    cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
    cJSON_AddNumberToObject(root, "channel", ap_info.primary);
    cJSON_AddStringToObject(root, "auth_mode", cap_system_wifi_auth_mode_to_str(ap_info.authmode));

    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
        cJSON_AddNumberToObject(root, "primary_channel", primary);
        cJSON_AddNumberToObject(root, "second_channel", second);
    } else {
        ESP_LOGW(TAG, "wifi channel failed");
    }

    return root;
}

static cJSON *cap_system_build_cpu_json(void)
{
    cJSON *root = cJSON_CreateObject();

    if (!root) {
        ESP_LOGE(TAG, "cpu json: create failed");
        return NULL;
    }

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_USE_TRACE_FACILITY
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = NULL;
    UBaseType_t count = 0;
    uint32_t total_runtime = 0;
    uint64_t idle_runtime = 0;
    double usage_percent = 0;

    tasks = calloc((size_t)task_count + 4, sizeof(TaskStatus_t));
    if (!tasks) {
        ESP_LOGE(TAG, "cpu stats: no mem");
        cJSON_Delete(root);
        return NULL;
    }

    count = uxTaskGetSystemState(tasks, task_count + 4, &total_runtime);
    for (UBaseType_t i = 0; i < count; i++) {
        if (strncmp(tasks[i].pcTaskName, "IDLE", 4) == 0) {
            idle_runtime += tasks[i].ulRunTimeCounter;
        }
    }

    if (total_runtime > 0 && idle_runtime <= total_runtime) {
        usage_percent = 100.0 - (((double)idle_runtime * 100.0) / (double)total_runtime);
    }
    if (usage_percent < 0) {
        usage_percent = 0;
    }
    if (usage_percent > 100.0) {
        usage_percent = 100.0;
    }

    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddNumberToObject(root, "usage_percent", usage_percent);
    cJSON_AddNumberToObject(root, "task_count", count);
    cJSON_AddNumberToObject(root, "runtime_total_ticks", (double)total_runtime);
    cJSON_AddNumberToObject(root, "runtime_idle_ticks", (double)idle_runtime);
    free(tasks);
#else
    cJSON_AddBoolToObject(root, "supported", false);
    cJSON_AddStringToObject(root, "message", "FreeRTOS run time stats are disabled");
#endif

    cJSON_AddNumberToObject(root, "core_count", portNUM_PROCESSORS);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    return root;
}

static cJSON *cap_system_build_info_json(void)
{
    cJSON *root = cJSON_CreateObject();
    esp_chip_info_t chip_info = {0};

    if (!root) {
        ESP_LOGE(TAG, "system json: create failed");
        return NULL;
    }

    esp_chip_info(&chip_info);
    cJSON_AddStringToObject(root, "chip_model", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(root, "chip_revision", chip_info.revision);
    cJSON_AddNumberToObject(root, "core_count", chip_info.cores);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddItemToObject(root, "memory", cap_system_build_memory_json());
    cJSON_AddItemToObject(root, "cpu", cap_system_build_cpu_json());
    cJSON_AddItemToObject(root, "wifi", cap_system_build_wifi_json());
    cJSON_AddItemToObject(root, "ip", cap_system_build_ip_json());
    return root;
}

static esp_err_t cap_system_write_json(cJSON *(*builder)(void), char *output, size_t output_size)
{
    cJSON *root = NULL;
    esp_err_t err;

    if (!builder || !output || output_size == 0) {
        ESP_LOGE(TAG, "write json: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }

    root = builder();
    if (!root) {
        ESP_LOGE(TAG, "write json: build failed");
        return ESP_ERR_NO_MEM;
    }

    err = cap_system_render_json(root, output, output_size);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write json failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void cap_system_restart_task(void *arg)
{
    cap_system_restart_task_args_t *task_args = (cap_system_restart_task_args_t *)arg;
    uint32_t delay_ms = task_args ? task_args->delay_ms : CAP_SYSTEM_RESTART_DEFAULT_DELAY_MS;

    free(task_args);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

static esp_err_t cap_system_get_info_json(char *output, size_t output_size)
{
    esp_err_t err = cap_system_write_json(cap_system_build_info_json, output, output_size);

    if (err == ESP_OK) {
        CAP_SYSTEM_DEBUG_LOG("system_info", output);
    }

    return err;
}

static esp_err_t cap_system_get_memory_info_json(char *output, size_t output_size)
{
    esp_err_t err = cap_system_write_json(cap_system_build_memory_json, output, output_size);

    if (err == ESP_OK) {
        CAP_SYSTEM_DEBUG_LOG("memory_info", output);
    }

    return err;
}

static esp_err_t cap_system_get_cpu_info_json(char *output, size_t output_size)
{
    esp_err_t err = cap_system_write_json(cap_system_build_cpu_json, output, output_size);

    if (err == ESP_OK) {
        CAP_SYSTEM_DEBUG_LOG("cpu_info", output);
    }

    return err;
}

static esp_err_t cap_system_get_wifi_info_json(char *output, size_t output_size)
{
    esp_err_t err = cap_system_write_json(cap_system_build_wifi_json, output, output_size);

    if (err == ESP_OK) {
        CAP_SYSTEM_DEBUG_LOG("wifi_info", output);
    }

    return err;
}

static esp_err_t cap_system_get_ip_info_json(char *output, size_t output_size)
{
    esp_err_t err = cap_system_write_json(cap_system_build_ip_json, output, output_size);

    if (err == ESP_OK) {
        CAP_SYSTEM_DEBUG_LOG("ip_info", output);
    }

    return err;
}

static esp_err_t cap_system_restart_async(uint32_t delay_ms)
{
    cap_system_restart_task_args_t *task_args = NULL;
    BaseType_t ok;

    if (delay_ms == 0) {
        delay_ms = CAP_SYSTEM_RESTART_DEFAULT_DELAY_MS;
    }

    task_args = calloc(1, sizeof(*task_args));
    if (!task_args) {
        ESP_LOGE(TAG, "restart args: no mem");
        return ESP_ERR_NO_MEM;
    }
    task_args->delay_ms = delay_ms;

    // Restart is deferred to let the current response flush out first.
    ok = claw_task_create(&(claw_task_config_t){
                              .name = "cap_system_restart",
                              .stack_size = 3072,
                              .priority = 5,
                              .core_id = tskNO_AFFINITY,
                              .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                          },
                          cap_system_restart_task,
                          task_args,
                          NULL);
    if (ok != pdPASS) {
        free(task_args);
        ESP_LOGE(TAG, "restart task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGW(TAG, "Device restart scheduled in %" PRIu32 " ms", delay_ms);
    return ESP_OK;
}

static esp_err_t cap_system_execute_get_info(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    (void)input_json;
    (void)ctx;
    return cap_system_get_info_json(output, output_size);
}

static esp_err_t cap_system_execute_get_memory(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    (void)input_json;
    (void)ctx;
    return cap_system_get_memory_info_json(output, output_size);
}

static esp_err_t cap_system_execute_get_cpu(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    (void)input_json;
    (void)ctx;
    return cap_system_get_cpu_info_json(output, output_size);
}

static esp_err_t cap_system_execute_get_wifi(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    (void)input_json;
    (void)ctx;
    return cap_system_get_wifi_info_json(output, output_size);
}

static esp_err_t cap_system_execute_get_ip(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output,
                                           size_t output_size)
{
    (void)input_json;
    (void)ctx;
    return cap_system_get_ip_info_json(output, output_size);
}

static esp_err_t cap_system_execute_restart(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *input = NULL;
    cJSON *delay_item = NULL;
    uint32_t delay_ms = CAP_SYSTEM_RESTART_DEFAULT_DELAY_MS;
    esp_err_t err;

    (void)ctx;

    if (!output || output_size == 0) {
        ESP_LOGE(TAG, "restart cmd: invalid output");
        return ESP_ERR_INVALID_ARG;
    }

    if (input_json && input_json[0]) {
        input = cJSON_Parse(input_json);
        if (!input || !cJSON_IsObject(input)) {
            ESP_LOGE(TAG, "restart cmd: invalid json");
            cJSON_Delete(input);
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid input json\"}");
            return ESP_ERR_INVALID_ARG;
        }

        delay_item = cJSON_GetObjectItem(input, "delay_ms");
        if (cJSON_IsNumber(delay_item) && delay_item->valuedouble >= 0) {
            delay_ms = (uint32_t)delay_item->valuedouble;
        }
    }

    err = cap_system_restart_async(delay_ms);
    cJSON_Delete(input);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "restart schedule failed: %s", esp_err_to_name(err));
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"failed to schedule restart\",\"code\":\"%s\"}",
                 esp_err_to_name(err));
        return err;
    }

    snprintf(output,
             output_size,
             "{\"ok\":true,\"message\":\"device restart scheduled\",\"delay_ms\":%" PRIu32 "}",
             delay_ms);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_system_descriptors[] = {
    {
        .id = "get_system_info",
        .name = "get_system_info",
        .family = "system",
        .description = "Get system or device summary including memory, CPU, Wi-Fi, IP, chip, and uptime information. **You cannot speculate or fabricate information.**",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_system_execute_get_info,
    },
    {
        .id = "get_memory_info",
        .name = "get_memory_info",
        .family = "system",
        .description = "Get internal RAM, heap, and PSRAM usage information.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_system_execute_get_memory,
    },
    {
        .id = "get_cpu_usage",
        .name = "get_cpu_usage",
        .family = "system",
        .description = "Get overall CPU usage derived from FreeRTOS run time statistics.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_system_execute_get_cpu,
    },
    {
        .id = "get_wifi_info",
        .name = "get_wifi_info",
        .family = "system",
        .description = "Get current Wi-Fi connection details such as SSID(name), RSSI, and channel.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_system_execute_get_wifi,
    },
    {
        .id = "get_ip_address",
        .name = "get_ip_address",
        .family = "system",
        .description = "Get current station IP, netmask, and gateway information.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_system_execute_get_ip,
    },
    {
        .id = "restart_device",
        .name = "restart_device",
        .family = "system",
        .description = "Schedule a device restart after an optional delay in milliseconds.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"delay_ms\":{\"type\":\"integer\",\"minimum\":0}}}",
        .execute = cap_system_execute_restart,
    },
};

static const claw_cap_group_t s_system_group = {
    .group_id = "cap_system",
    .descriptors = s_system_descriptors,
    .descriptor_count = sizeof(s_system_descriptors) / sizeof(s_system_descriptors[0]),
};

esp_err_t cap_system_register_group(void)
{
    if (claw_cap_group_exists(s_system_group.group_id)) {
        return ESP_OK;
    }

    esp_err_t err = claw_cap_register_group(&s_system_group);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register group failed: %s", esp_err_to_name(err));
    }

    return err;
}
