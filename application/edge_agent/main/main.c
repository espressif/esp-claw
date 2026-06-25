/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "app_fs.h"
#include "claw_event_router.h"
#include "claw_version.h"
#include "claw_paths.h"
#include "edge_agent_version.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "wifi_manager.h"
#include "time.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_board_manager_includes.h"
#include "captive_dns.h"
#include "cmd_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#include "app_config.h"

#define APP_ENABLE_MEM_LOG        (0)

static const char *TAG = "app";

#define RUNTIME_GUARD_NAMESPACE      "runtime_guard"
#define RUNTIME_GUARD_STAGE_KEY      "stage"
#define RUNTIME_GUARD_SAFE_KEY       "safe"
#define RUNTIME_GUARD_REASON_KEY     "reason"
#define RUNTIME_GUARD_STAGE_HTTP     "booting_http"
#define RUNTIME_GUARD_STAGE_BUSINESS "business"
#define RUNTIME_GUARD_STAGE_RUNNING  "running"
#define RUNTIME_GUARD_STAGE_SAFE     "safe_mode"

static app_config_t *s_config;
static app_claw_config_t *s_claw_config;
static bool s_safe_mode;
static char s_safe_mode_reason[96];
static char s_reset_reason[32];

static esp_err_t app_allocate_runtime_state(void)
{
    if (!s_config) {
        s_config = calloc(1, sizeof(*s_config));
    }
    if (!s_claw_config) {
        s_claw_config = calloc(1, sizeof(*s_claw_config));
    }

    ESP_RETURN_ON_FALSE(s_config && s_claw_config, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate runtime state");

    return ESP_OK;
}

static void app_free_runtime_state(void)
{
    free(s_claw_config);
    s_claw_config = NULL;

    free(s_config);
    s_config = NULL;
}

static const char *main_reset_reason_to_string(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_wdt";
    case ESP_RST_TASK_WDT:
        return "task_wdt";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_UNKNOWN:
    default:
        return "unknown";
    }
}

static bool main_reset_reason_is_business_crash(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC ||
           reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT;
}

static esp_err_t main_runtime_guard_set_stage(const char *stage)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(RUNTIME_GUARD_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, RUNTIME_GUARD_STAGE_KEY, stage ? stage : "");
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t main_runtime_guard_init(void)
{
    nvs_handle_t handle;
    esp_err_t err;
    char last_stage[32] = {0};
    size_t last_stage_len = sizeof(last_stage);
    char persisted_reason[sizeof(s_safe_mode_reason)] = {0};
    size_t persisted_reason_len = sizeof(persisted_reason);
    uint8_t persisted_safe = 0;
    esp_reset_reason_t reset_reason = esp_reset_reason();

    strlcpy(s_reset_reason, main_reset_reason_to_string(reset_reason), sizeof(s_reset_reason));

    err = nvs_open(RUNTIME_GUARD_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "runtime guard NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    if (nvs_get_str(handle, RUNTIME_GUARD_STAGE_KEY, last_stage, &last_stage_len) != ESP_OK) {
        last_stage[0] = '\0';
    }
    if (nvs_get_u8(handle, RUNTIME_GUARD_SAFE_KEY, &persisted_safe) != ESP_OK) {
        persisted_safe = 0;
    }
    if (nvs_get_str(handle, RUNTIME_GUARD_REASON_KEY,
                    persisted_reason, &persisted_reason_len) != ESP_OK) {
        persisted_reason[0] = '\0';
    }

    if (persisted_safe) {
        s_safe_mode = true;
        strlcpy(s_safe_mode_reason,
                persisted_reason[0] ? persisted_reason : "safe mode requested",
                sizeof(s_safe_mode_reason));
    } else if (main_reset_reason_is_business_crash(reset_reason) &&
               (strcmp(last_stage, RUNTIME_GUARD_STAGE_BUSINESS) == 0 ||
                strcmp(last_stage, RUNTIME_GUARD_STAGE_RUNNING) == 0)) {
        s_safe_mode = true;
        snprintf(s_safe_mode_reason,
                 sizeof(s_safe_mode_reason),
                 "previous reset=%s during %s",
                 s_reset_reason,
                 last_stage[0] ? last_stage : "business");
        (void)nvs_set_u8(handle, RUNTIME_GUARD_SAFE_KEY, 1);
        (void)nvs_set_str(handle, RUNTIME_GUARD_REASON_KEY, s_safe_mode_reason);
    } else {
        s_safe_mode = false;
        s_safe_mode_reason[0] = '\0';
    }

    (void)nvs_set_str(handle,
                      RUNTIME_GUARD_STAGE_KEY,
                      s_safe_mode ? RUNTIME_GUARD_STAGE_SAFE : RUNTIME_GUARD_STAGE_HTTP);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (s_safe_mode) {
        ESP_LOGW(TAG, "Runtime safe mode active: %s", s_safe_mode_reason);
    }
    return err;
}

static esp_err_t main_runtime_guard_clear_safe_mode(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(RUNTIME_GUARD_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    (void)nvs_erase_key(handle, RUNTIME_GUARD_SAFE_KEY);
    (void)nvs_erase_key(handle, RUNTIME_GUARD_REASON_KEY);
    err = nvs_set_str(handle, RUNTIME_GUARD_STAGE_KEY, RUNTIME_GUARD_STAGE_HTTP);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static const char *main_router_health_state_to_string(claw_event_router_health_state_t state)
{
    switch (state) {
    case CLAW_EVENT_ROUTER_HEALTH_OK:
        return "ok";
    case CLAW_EVENT_ROUTER_HEALTH_DEGRADED:
        return "degraded";
    case CLAW_EVENT_ROUTER_HEALTH_STOPPED:
        return "stopped";
    case CLAW_EVENT_ROUTER_HEALTH_SAFE_MODE_DISABLED:
        return "safe_mode_disabled";
    default:
        return "unknown";
    }
}

static void log_wifi_startup_config(const app_config_t *config)
{
    ESP_LOGI(TAG,
             "Wi-Fi startup STA: ssid=%s pwd_len=%u",
             config->wifi_ssid[0] ? config->wifi_ssid : "(empty)",
             (unsigned)strlen(config->wifi_password));

    ESP_LOGI(TAG,
             "Wi-Fi startup AP: ssid=%s pwd_len=%u behavior=%s",
             config->ap_ssid[0] ? config->ap_ssid : "(auto:mac-suffix)",
             (unsigned)strlen(config->ap_password),
             config->ap_behavior[0] ? config->ap_behavior : "keep");
}

static void on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    const char *ap_ssid = status.ap_active ? status.ap_ssid : NULL;

    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s",
             connected,
             status.ap_active,
             status.mode ? status.mode : "off",
             ap_ssid ? ap_ssid : "(none)");

    esp_err_t err = app_claw_set_network_status(connected, ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update network emote: %s", esp_err_to_name(err));
    }
}

static esp_err_t main_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static esp_err_t main_save_config(const app_config_t *config)
{
    esp_err_t err;
    app_claw_config_t *claw_config = NULL;

    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(app_config_validate_wifi(config, NULL), TAG, "Invalid Wi-Fi config");

    err = app_config_save(config);
    if (err != ESP_OK) {
        return err;
    }

    claw_config = calloc(1, sizeof(*claw_config));
    if (!claw_config) {
        ESP_LOGW(TAG, "Failed to allocate Claw config for runtime update");
        return ESP_OK;
    }
    app_config_to_claw(config, claw_config);
    err = app_claw_update_config(claw_config);
    free(claw_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to update running Claw config: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void main_copy_claw_to_app_config(const app_claw_config_t *src, app_config_t *dst)
{
    strlcpy(dst->llm_api_key, src->llm_api_key, sizeof(dst->llm_api_key));
    strlcpy(dst->llm_backend_type, src->llm_backend_type, sizeof(dst->llm_backend_type));
    strlcpy(dst->llm_model, src->llm_model, sizeof(dst->llm_model));
    strlcpy(dst->llm_base_url, src->llm_base_url, sizeof(dst->llm_base_url));
    strlcpy(dst->llm_auth_type, src->llm_auth_type, sizeof(dst->llm_auth_type));
    strlcpy(dst->llm_timeout_ms, src->llm_timeout_ms, sizeof(dst->llm_timeout_ms));
    strlcpy(dst->llm_max_tokens, src->llm_max_tokens, sizeof(dst->llm_max_tokens));
    strlcpy(dst->llm_default_image_max_bytes,
            src->llm_default_image_max_bytes,
            sizeof(dst->llm_default_image_max_bytes));
    strlcpy(dst->llm_max_tokens_field, src->llm_max_tokens_field, sizeof(dst->llm_max_tokens_field));
    strlcpy(dst->llm_supports_tools, src->llm_supports_tools, sizeof(dst->llm_supports_tools));
    strlcpy(dst->llm_supports_vision, src->llm_supports_vision, sizeof(dst->llm_supports_vision));
    strlcpy(dst->llm_image_remote_url_only,
            src->llm_image_remote_url_only,
            sizeof(dst->llm_image_remote_url_only));
}

static esp_err_t main_save_claw_config(const app_claw_config_t *config, void *user_ctx)
{
    esp_err_t err;
    app_config_t *app_config = NULL;

    (void)user_ctx;
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    app_config = calloc(1, sizeof(*app_config));
    ESP_RETURN_ON_FALSE(app_config, ESP_ERR_NO_MEM, TAG, "Failed to allocate app config for Claw save");

    err = app_config_load(app_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load config for Claw save: %s", esp_err_to_name(err));
        free(app_config);
        return err;
    }
    main_copy_claw_to_app_config(config, app_config);
    err = app_config_save(app_config);
    free(app_config);
    return err;
}

static esp_err_t main_get_wifi_status(http_server_wifi_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    status->ap_ip = wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;
    return ESP_OK;
}

static esp_err_t main_get_runtime_status(http_server_runtime_status_t *status)
{
    claw_event_router_health_t router = {0};
    esp_err_t err;

    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");
    memset(status, 0, sizeof(*status));
    status->safe_mode = s_safe_mode;
    strlcpy(status->safe_mode_reason, s_safe_mode_reason, sizeof(status->safe_mode_reason));
    strlcpy(status->reset_reason, s_reset_reason, sizeof(status->reset_reason));

    if (s_safe_mode) {
        status->router_available = false;
        strlcpy(status->router_state, "safe_mode_disabled", sizeof(status->router_state));
        strlcpy(status->router_reason,
                s_safe_mode_reason[0] ? s_safe_mode_reason : "safe mode active",
                sizeof(status->router_reason));
        return ESP_OK;
    }

    err = claw_event_router_get_health(&router);
    if (err != ESP_OK) {
        status->router_available = false;
        strlcpy(status->router_state, "stopped", sizeof(status->router_state));
        strlcpy(status->router_reason, "router not initialized", sizeof(status->router_reason));
        status->router_last_error = err;
        return ESP_OK;
    }

    status->router_available = router.state == CLAW_EVENT_ROUTER_HEALTH_OK;
    strlcpy(status->router_state,
            main_router_health_state_to_string(router.state),
            sizeof(status->router_state));
    strlcpy(status->router_reason, router.reason, sizeof(status->router_reason));
    status->router_event_queue_depth = router.event_queue_depth;
    status->router_action_queue_depth = router.action_queue_depth;
    status->router_stack_hwm_bytes = router.router_stack_hwm_bytes;
    status->router_action_stack_hwm_bytes = router.action_stack_hwm_bytes;
    status->router_failed_actions = router.failed_actions;
    status->router_dropped_events = router.dropped_events;
    status->router_last_error = router.last_error;
    return ESP_OK;
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create restart task");
    return ESP_OK;
}

static esp_err_t main_clear_safe_mode(void)
{
    ESP_RETURN_ON_ERROR(main_runtime_guard_clear_safe_mode(), TAG, "Failed to clear runtime guard");
    s_safe_mode = false;
    s_safe_mode_reason[0] = '\0';
    return main_restart_device();
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t main_wechat_login_start(const char *account_id, bool force)
{
    return cap_im_wechat_qr_login_start(account_id, force);
}

static esp_err_t main_wechat_login_get_status(http_server_wechat_login_status_t *status)
{
    esp_err_t ret = ESP_OK;
    cap_im_wechat_qr_login_status_t *raw = NULL;

    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    raw = calloc(1, sizeof(*raw));
    ESP_RETURN_ON_FALSE(raw, ESP_ERR_NO_MEM, TAG, "Failed to allocate login status");

    ESP_GOTO_ON_ERROR(cap_im_wechat_qr_login_get_status(raw), cleanup, TAG,
                      "Failed to query WeChat login status");

    memset(status, 0, sizeof(*status));
    status->active = raw->active;
    status->configured = raw->configured;
    status->completed = raw->completed;
    status->persisted = raw->persisted;
    strlcpy(status->session_key, raw->session_key, sizeof(status->session_key));
    strlcpy(status->status, raw->status, sizeof(status->status));
    strlcpy(status->message, raw->message, sizeof(status->message));
    strlcpy(status->qr_data_url, raw->qr_data_url, sizeof(status->qr_data_url));
    strlcpy(status->account_id, raw->account_id, sizeof(status->account_id));
    strlcpy(status->user_id, raw->user_id, sizeof(status->user_id));
    strlcpy(status->token, raw->token, sizeof(status->token));
    strlcpy(status->base_url, raw->base_url, sizeof(status->base_url));

cleanup:
    free(raw);
    return ret;
}

static esp_err_t main_wechat_login_cancel(void)
{
    return cap_im_wechat_qr_login_cancel();
}

static esp_err_t main_wechat_login_mark_persisted(void)
{
    return cap_im_wechat_qr_login_mark_persisted();
}
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(timezone && timezone[0] != '\0', ESP_ERR_INVALID_ARG, tz_default, TAG,
                      "Timezone is empty.");
    ESP_GOTO_ON_FALSE(setenv("TZ", timezone, 1) == 0, ESP_FAIL, tz_default, TAG,
                      "Failed to set TZ env");
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return ret;
}

#if APP_ENABLE_MEM_LOG

static void print_task_stack_info(void)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t s_task_status_snapshot[24];
    UBaseType_t count = uxTaskGetSystemState(s_task_status_snapshot,
                                             sizeof(s_task_status_snapshot) / sizeof(s_task_status_snapshot[0]),
                                             NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG,
                 "Task %s  %u",
                 s_task_status_snapshot[i].pcTaskName,
                 s_task_status_snapshot[i].usStackHighWaterMark);
    }
#endif
}

/* Periodic task: print internal free, minimum free, and PSRAM free every 20s */
static void memory_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Memory: internal_free=%u bytes, internal_min_free=%u bytes, psram_free=%u bytes",
                 (unsigned)internal_free, (unsigned)internal_min, (unsigned)psram_free);
        print_task_stack_info();
    }
}

#endif

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("http_reuse", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_LOGI(TAG, "ESP-Claw version: %s", claw_get_version());
    ESP_LOGI(TAG, "ESP-Claw git version: %s", claw_get_git_version());
    ESP_LOGI(TAG, "Edge Agent version: %s", edge_agent_get_version());
    ESP_ERROR_CHECK(app_allocate_runtime_state());
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(main_runtime_guard_init());
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(app_config_load(s_config));
    app_config_to_claw(s_config, s_claw_config);
    init_timezone(app_config_get_timezone(s_config)); // no need to check error
    ESP_ERROR_CHECK(esp_board_manager_init());
    ESP_ERROR_CHECK(app_claw_ui_start());
    ESP_ERROR_CHECK(app_fs_init());

    /* Publish the resolved storage roots so any component can compose paths
     * without knowing whether data lives on flash or an SD card. */
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_DATA, app_fs_storage_base_path()));
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_SYSTEM, app_fs_system_base_path()));

    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fs_storage_base_path(),
        .services = {
            .load_config = main_load_config,
            .save_config = main_save_config,
            .get_wifi_status = main_get_wifi_status,
            .get_runtime_status = main_get_runtime_status,
            .clear_safe_mode = main_clear_safe_mode,
            .restart_device = main_restart_device,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
            .wechat_login_start = main_wechat_login_start,
            .wechat_login_get_status = main_wechat_login_get_status,
            .wechat_login_cancel = main_wechat_login_cancel,
            .wechat_login_mark_persisted = main_wechat_login_mark_persisted,
#endif
        },
    }));
    ESP_ERROR_CHECK(wifi_manager_register_state_callback(on_wifi_state_changed, NULL));

    log_wifi_startup_config(s_config);

    esp_err_t wifi_err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = s_config->wifi_ssid,
        .sta_password = s_config->wifi_password,
        .ap_ssid = s_config->ap_ssid[0] ? s_config->ap_ssid : NULL,
        .ap_password = s_config->ap_password[0] ? s_config->ap_password : NULL,
        .ap_behavior = s_config->ap_behavior,
    });
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        ESP_ERROR_CHECK(http_server_start());
        if (captive_dns_start(&(captive_dns_config_t) {
                .ap_netif = wifi_manager_get_ap_netif(),
                .configure_dhcp_dns = true,
            }) != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
        }

        if (s_config->wifi_ssid[0] != '\0') {
            esp_err_t wait_err = wifi_manager_wait_connected(30000);
            if (wait_err == ESP_OK) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGI(TAG, "Wi-Fi STA ready: %s", status.sta_ip);
            } else if (wait_err == ESP_FAIL) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGW(TAG,
                         "Wi-Fi STA failed after retries: mode=%s ap_active=%d ap_ip=%s",
                         status.mode ? status.mode : "off",
                         status.ap_active,
                         status.ap_ip ? status.ap_ip : "0.0.0.0");
            } else if (wait_err == ESP_ERR_TIMEOUT) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGW(TAG,
                         "Wi-Fi STA wait timeout: mode=%s ap_active=%d sta_configured=%d",
                         status.mode ? status.mode : "off",
                         status.ap_active,
                         status.sta_configured);
            } else {
                ESP_LOGW(TAG, "Wi-Fi STA wait returned error: %s", esp_err_to_name(wait_err));
            }
        }

        wifi_manager_status_t status = {0};
        wifi_manager_get_status(&status);
        if (status.ap_active) {
            const char *portal_auth = s_config->ap_password[0] ? "wpa2" : "open";
            ESP_LOGW(TAG,
                     "*** Provisioning portal: SSID=\"%s\" (auth=%s) IP=%s URL=http://%s/ ***",
                     status.ap_ssid,
                     portal_auth,
                     status.ap_ip,
                     status.ap_ip);
        }
    }

    ESP_ERROR_CHECK(app_claw_set_save_config_callback(main_save_claw_config, NULL));
    if (s_safe_mode) {
        ESP_LOGW(TAG, "Safe mode: skipping router/agent/webim startup");
    } else {
        ESP_ERROR_CHECK(main_runtime_guard_set_stage(RUNTIME_GUARD_STAGE_BUSINESS));
        ESP_ERROR_CHECK(app_claw_start(s_claw_config));
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
        ESP_ERROR_CHECK(http_server_webim_bind_im());
#endif
        ESP_ERROR_CHECK(main_runtime_guard_set_stage(RUNTIME_GUARD_STAGE_RUNNING));
    }

    register_wifi_command();

#if APP_ENABLE_MEM_LOG
    /* Start memory monitor: print internal free, min free, PSRAM free every 20s */
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif

    app_free_runtime_state();
}
