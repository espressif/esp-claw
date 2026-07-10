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
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_version.h"
#include "claw_task.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cap_system";

#define CAP_SYSTEM_RESTART_DEFAULT_DELAY_MS 500
#define CAP_SYSTEM_MIN_VALID_EPOCH          1704067200
#define CAP_SYSTEM_SNTP_PRIMARY_SERVER      "pool.ntp.org"
#define CAP_SYSTEM_SNTP_SECONDARY_SERVER    "time.windows.com"
#define CAP_SYSTEM_SNTP_WAIT_MS             3000
#define CAP_SYSTEM_SNTP_RETRY_COUNT         15
#define CAP_SYSTEM_DEFAULT_DISCONNECTED_RETRY_MS 5000
#define CAP_SYSTEM_DEFAULT_SYNC_RETRY_MS        30000

#ifdef CONFIG_CLAW_CAP_SYSTEM_DEBUG_LOGS
#define CAP_SYSTEM_DEBUG_LOG(label, text) ESP_LOGI(TAG, "%s: %s", label, text)
#else
#define CAP_SYSTEM_DEBUG_LOG(label, text) do { (void)(label); (void)(text); } while (0)
#endif

typedef struct {
    uint32_t delay_ms;
} cap_system_restart_task_args_t;

static SemaphoreHandle_t s_time_mutex = NULL;
static struct {
    TaskHandle_t task_handle;
    cap_system_time_sync_service_config_t config;
    bool running;
} s_time_service = {0};

static esp_err_t cap_system_get_current_time(char *output, size_t output_size);
static esp_err_t cap_system_sync_time_now(char *output, size_t output_size);
static bool cap_system_is_time_valid(void);

static esp_err_t cap_system_ensure_time_mutex(void)
{
    if (!s_time_mutex) {
        s_time_mutex = xSemaphoreCreateMutex();
    }
    return s_time_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

static void cap_system_time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "SNTP time synchronization event received");
}

static esp_err_t cap_system_format_current_time(char *output, size_t output_size)
{
    time_t now = 0;
    struct tm local_tm = {0};

    if (!output || output_size == 0) {
        ESP_LOGE(TAG, "format current time: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }

    time(&now);
    if (now < CAP_SYSTEM_MIN_VALID_EPOCH) {
        ESP_LOGW(TAG, "format current time: clock is invalid");
        return ESP_ERR_INVALID_STATE;
    }
    if (!localtime_r(&now, &local_tm)) {
        ESP_LOGE(TAG, "format current time: localtime_r failed");
        return ESP_FAIL;
    }
    if (strftime(output, output_size, "%Y-%m-%d %H:%M:%S %Z (%A)", &local_tm) == 0) {
        ESP_LOGE(TAG, "format current time: buffer too small");
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cap_system_sync_with_sntp(char *output, size_t output_size)
{
    esp_err_t err = ESP_OK;
    esp_err_t wait_err = ESP_OK;
    int retry = 0;
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2,
        ESP_SNTP_SERVER_LIST(CAP_SYSTEM_SNTP_PRIMARY_SERVER, CAP_SYSTEM_SNTP_SECONDARY_SERVER)
    );
#else
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CAP_SYSTEM_SNTP_PRIMARY_SERVER);
#endif

    if (!output || output_size == 0) {
        ESP_LOGE(TAG, "sync with sntp: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }

    config.sync_cb = cap_system_time_sync_notification_cb;
    err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sync with sntp init failed: %s", esp_err_to_name(err));
        return err;
    }

    while ((wait_err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(CAP_SYSTEM_SNTP_WAIT_MS))) == ESP_ERR_TIMEOUT &&
           ++retry < CAP_SYSTEM_SNTP_RETRY_COUNT) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, CAP_SYSTEM_SNTP_RETRY_COUNT);
    }

    if (wait_err != ESP_OK) {
        err = wait_err;
        ESP_LOGE(TAG, "sync with sntp wait failed: %s", esp_err_to_name(err));
        goto done;
    }

    err = cap_system_format_current_time(output, output_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sync with sntp format failed: %s", esp_err_to_name(err));
    }

done:
    esp_netif_sntp_deinit();
    return err;
}

static bool cap_system_time_network_ready(void)
{
    if (!s_time_service.config.network_ready) {
        return true;
    }

    return s_time_service.config.network_ready(s_time_service.config.network_ready_ctx);
}

static void cap_system_notify_time_sync_success(bool had_valid_time)
{
    if (s_time_service.config.on_sync_success) {
        s_time_service.config.on_sync_success(had_valid_time, s_time_service.config.on_sync_success_ctx);
    }
}

static void cap_system_time_sync_service_task(void *arg)
{
    char output[256];

    (void)arg;

    while (s_time_service.running) {
        bool time_valid;
        uint32_t delay_ms;

        if (!cap_system_time_network_ready()) {
            delay_ms = s_time_service.config.disconnected_retry_ms;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue;
        }

        time_valid = cap_system_is_time_valid();
        if (!time_valid) {
            esp_err_t err;

            output[0] = '\0';
            err = cap_system_sync_time_now(output, sizeof(output));
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Time sync succeeded: %s", output);
                cap_system_notify_time_sync_success(false);
                break;
            }

            ESP_LOGW(TAG, "Time sync failed: %s", esp_err_to_name(err));
            delay_ms = s_time_service.config.sync_retry_ms;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue;
        }

        cap_system_notify_time_sync_success(false);
        break;
    }

    s_time_service.running = false;
    s_time_service.task_handle = NULL;
    claw_task_delete(NULL);
}

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

static cJSON *cap_system_build_version_json(void)
{
    cJSON *root = cJSON_CreateObject();

    if (!root) {
        ESP_LOGE(TAG, "version json: create failed");
        return NULL;
    }

    // ESP-Claw comes from claw_core; Edge Agent comes from ESP-IDF PROJECT_VER embedded in esp_app_desc.
    cJSON_AddStringToObject(root, "esp_claw", claw_get_version());
    cJSON_AddStringToObject(root, "esp_claw_git", claw_get_git_version());
    cJSON_AddStringToObject(root, "edge_agent", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "esp_idf", esp_get_idf_version());
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
    cJSON_AddItemToObject(root, "version", cap_system_build_version_json());
    cJSON_AddItemToObject(root, "memory", cap_system_build_memory_json());
    cJSON_AddItemToObject(root, "cpu", cap_system_build_cpu_json());
    cJSON_AddItemToObject(root, "wifi", cap_system_build_wifi_json());
    cJSON_AddItemToObject(root, "ip", cap_system_build_ip_json());
    return root;
}

static bool cap_system_section_is_requested(const cJSON *sections, const char *section)
{
    const cJSON *item = NULL;

    if (!sections || !section) {
        return false;
    }

    cJSON_ArrayForEach(item, sections) {
        if (cJSON_IsString(item) && item->valuestring && strcmp(item->valuestring, section) == 0) {
            return true;
        }
    }

    return false;
}

static esp_err_t cap_system_add_json_section(cJSON *root, const char *name, cJSON *(*builder)(void))
{
    cJSON *section = NULL;

    if (!root || !name || !builder) {
        ESP_LOGE(TAG, "add json section: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }

    section = builder();
    if (!section) {
        ESP_LOGE(TAG, "add json section failed: %s", name);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToObject(root, name, section);
    return ESP_OK;
}

static esp_err_t cap_system_add_chip_section(cJSON *root)
{
    cJSON *chip = NULL;
    esp_chip_info_t chip_info = {0};

    if (!root) {
        ESP_LOGE(TAG, "add chip section: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }

    chip = cJSON_CreateObject();
    if (!chip) {
        ESP_LOGE(TAG, "chip section: create failed");
        return ESP_ERR_NO_MEM;
    }

    esp_chip_info(&chip_info);
    cJSON_AddStringToObject(chip, "chip_model", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(chip, "chip_revision", chip_info.revision);
    cJSON_AddNumberToObject(chip, "core_count", chip_info.cores);
    cJSON_AddItemToObject(root, "chip", chip);
    return ESP_OK;
}

static esp_err_t cap_system_add_uptime_section(cJSON *root)
{
    if (!root) {
        ESP_LOGE(TAG, "add uptime section: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    return ESP_OK;
}

static esp_err_t cap_system_build_selected_info_json(const cJSON *sections, char *output, size_t output_size)
{
    cJSON *root = NULL;
    esp_err_t ret = ESP_OK;

    if (!sections || !cJSON_IsArray(sections) || cJSON_GetArraySize((cJSON *)sections) == 0 || !output || output_size == 0) {
        ESP_LOGE(TAG, "selected info json: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "selected info json: create failed");
        return ESP_ERR_NO_MEM;
    }

    // Keep section names aligned with the LLM schema enum to avoid ambiguous tool behavior.
    if (cap_system_section_is_requested(sections, "chip")) {
        ESP_GOTO_ON_ERROR(cap_system_add_chip_section(root), cleanup, TAG, "Failed to add chip section");
    }
    if (cap_system_section_is_requested(sections, "uptime")) {
        ESP_GOTO_ON_ERROR(cap_system_add_uptime_section(root), cleanup, TAG, "Failed to add uptime section");
    }
    if (cap_system_section_is_requested(sections, "version")) {
        ESP_GOTO_ON_ERROR(cap_system_add_json_section(root, "version", cap_system_build_version_json), cleanup, TAG, "Failed to add version section");
    }
    if (cap_system_section_is_requested(sections, "memory")) {
        ESP_GOTO_ON_ERROR(cap_system_add_json_section(root, "memory", cap_system_build_memory_json), cleanup, TAG, "Failed to add memory section");
    }
    if (cap_system_section_is_requested(sections, "cpu")) {
        ESP_GOTO_ON_ERROR(cap_system_add_json_section(root, "cpu", cap_system_build_cpu_json), cleanup, TAG, "Failed to add cpu section");
    }
    if (cap_system_section_is_requested(sections, "wifi")) {
        ESP_GOTO_ON_ERROR(cap_system_add_json_section(root, "wifi", cap_system_build_wifi_json), cleanup, TAG, "Failed to add wifi section");
    }
    if (cap_system_section_is_requested(sections, "ip")) {
        ESP_GOTO_ON_ERROR(cap_system_add_json_section(root, "ip", cap_system_build_ip_json), cleanup, TAG, "Failed to add ip section");
    }

    if (cJSON_GetArraySize(root) == 0) {
        ESP_LOGE(TAG, "selected info json: no known sections requested");
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"no known sections requested\"}");
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    ret = cap_system_render_json(root, output, output_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "selected info json render failed: %s", esp_err_to_name(ret));
    }

cleanup:
    cJSON_Delete(root);
    return ret;
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

static esp_err_t cap_system_get_selected_info_json(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = NULL;
    cJSON *sections = NULL;
    esp_err_t err = ESP_OK;

    if (!output || output_size == 0) {
        ESP_LOGE(TAG, "get selected info: invalid output");
        return ESP_ERR_INVALID_ARG;
    }

    if (!input_json || !input_json[0]) {
        return cap_system_get_info_json(output, output_size);
    }

    input = cJSON_Parse(input_json);
    if (!input || !cJSON_IsObject(input)) {
        ESP_LOGE(TAG, "get selected info: invalid json");
        cJSON_Delete(input);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid input json\"}");
        return ESP_ERR_INVALID_ARG;
    }

    sections = cJSON_GetObjectItem(input, "sections");
    if (!sections) {
        err = cap_system_get_info_json(output, output_size);
        goto cleanup;
    }
    if (!cJSON_IsArray(sections) || cJSON_GetArraySize(sections) == 0) {
        ESP_LOGE(TAG, "get selected info: invalid sections");
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"sections must be a non-empty array\"}");
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    err = cap_system_build_selected_info_json(sections, output, output_size);

    if (err == ESP_OK) {
        CAP_SYSTEM_DEBUG_LOG("selected_system_info", output);
    }

cleanup:
    cJSON_Delete(input);
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

static esp_err_t cap_system_get_current_time(char *output, size_t output_size)
{
    esp_err_t err;

    if (!output || output_size == 0) {
        ESP_LOGE(TAG, "get current time: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(cap_system_ensure_time_mutex(), TAG, "Failed to create time mutex");
    xSemaphoreTake(s_time_mutex, portMAX_DELAY);
    err = cap_system_is_time_valid() ? cap_system_format_current_time(output, output_size) : cap_system_sync_with_sntp(output, output_size);
    xSemaphoreGive(s_time_mutex);
    return err;
}

static esp_err_t cap_system_sync_time_now(char *output, size_t output_size)
{
    esp_err_t err;

    if (!output || output_size == 0) {
        ESP_LOGE(TAG, "sync time now: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(cap_system_ensure_time_mutex(), TAG, "Failed to create time mutex");
    xSemaphoreTake(s_time_mutex, portMAX_DELAY);
    err = cap_system_sync_with_sntp(output, output_size);
    xSemaphoreGive(s_time_mutex);
    return err;
}

static bool cap_system_is_time_valid(void)
{
    time_t now = time(NULL);

    return now >= CAP_SYSTEM_MIN_VALID_EPOCH;
}

esp_err_t cap_system_time_sync_service_start(const cap_system_time_sync_service_config_t *config)
{
    BaseType_t ok;

    if (!config) {
        ESP_LOGE(TAG, "time sync service start: invalid config");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_time_service.task_handle || s_time_service.running) {
        return ESP_OK;
    }

    memset(&s_time_service.config, 0, sizeof(s_time_service.config));
    s_time_service.config = *config;
    if (s_time_service.config.disconnected_retry_ms == 0) {
        s_time_service.config.disconnected_retry_ms = CAP_SYSTEM_DEFAULT_DISCONNECTED_RETRY_MS;
    }
    if (s_time_service.config.sync_retry_ms == 0) {
        s_time_service.config.sync_retry_ms = CAP_SYSTEM_DEFAULT_SYNC_RETRY_MS;
    }

    if (cap_system_is_time_valid()) {
        cap_system_notify_time_sync_success(false);
        return ESP_OK;
    }

    s_time_service.running = true;
    ok = claw_task_create(&(claw_task_config_t){
                              .name = "cap_system_time_sync",
                              .stack_size = 4096,
                              .priority = 5,
                              .core_id = tskNO_AFFINITY,
                              .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                          }, cap_system_time_sync_service_task, NULL, &s_time_service.task_handle);
    if (ok != pdPASS || !s_time_service.task_handle) {
        s_time_service.running = false;
        s_time_service.task_handle = NULL;
        ESP_LOGE(TAG, "time sync service start: task create failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t cap_system_execute_get_info(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    (void)ctx;
    return cap_system_get_selected_info_json(input_json, output, output_size);
}

static esp_err_t cap_system_execute_get_current_time(const char *input_json,
                                                     const claw_cap_call_context_t *ctx,
                                                     char *output,
                                                     size_t output_size)
{
    esp_err_t err;

    (void)input_json;
    (void)ctx;

    if (!output || output_size == 0) {
        ESP_LOGE(TAG, "get current time cmd: invalid output");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_system_get_current_time(output, output_size);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to get time (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", output);
        return err;
    }

    ESP_LOGI(TAG, "Current time: %s", output);
    return ESP_OK;
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
        .description = "Get system or device information. Omit sections for a full summary, or request selected sections: "
                       "chip, uptime, version, memory, cpu, wifi, ip. "
                       "**You cannot speculate or fabricate information.**",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"sections\":{\"type\":\"array\",\"items\":{\"type\":\"string\","
                             "\"enum\":[\"chip\",\"uptime\",\"version\",\"memory\",\"cpu\",\"wifi\",\"ip\"]},\"minItems\":1,\"uniqueItems\":true,"
                             "\"description\":\"Optional list of system sections to return. Omit for full summary.\"}}}",
        .execute = cap_system_execute_get_info,
    },
    {
        .id = "get_current_time",
        .name = "get_current_time",
        .family = "system",
        .description = "Return formatted current local time. Sync with SNTP only when the clock is invalid.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_system_execute_get_current_time,
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
