/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_claw.h"
#include "app_config.h"
#include "app_product_claw.h"
#include "app_provisioning.h"
#include "app_runtime_state.h"
#include "app_wechat_setup.h"
#include "claw_ramfs.h"
#include "cmd_wifi.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "nvs_flash.h"
#include "product_mode_controller.h"
#include "wear_levelling.h"

#define APP_ENABLE_MEM_LOG        (0)

#define APP_FATFS_PARTITION_LABEL "storage"
#define APP_RAMFS_BASE_PATH       "/ramfs"
#define APP_RAMFS_MAX_FILES       (8)
#define APP_RAMFS_MAX_BYTES       (512 * 1024)

static const char *TAG = "app";
static const char *app_fatfs_base_path = "/fatfs";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static esp_err_t init_timezone(const char *timezone);

static bool main_wifi_config_changed(const app_config_t *config)
{
    app_runtime_wifi_fields_t active = {0};

    if (config == NULL || app_runtime_state_wifi_fields_snapshot(&active) != ESP_OK) {
        return true;
    }

    return strcmp(active.sta_ssid, config->wifi_ssid) != 0 ||
           strcmp(active.sta_password, config->wifi_password) != 0 ||
           strcmp(active.ap_ssid, config->ap_ssid) != 0 ||
           strcmp(active.ap_password, config->ap_password) != 0 ||
           strcmp(active.ap_behavior, config->ap_behavior) != 0;
}

static esp_err_t main_save_config(const app_config_t *config)
{
    esp_err_t err;

    if (main_wifi_config_changed(config)) {
        err = app_provisioning_apply_wifi_config(config);
    } else {
        err = app_runtime_state_save_config(config);
    }
    if (err != ESP_OK) {
        return err;
    }

    (void)init_timezone(app_config_get_timezone(config));

    return app_product_claw_update_config(config,
                                          !app_runtime_state_business_ready_from_config(config));
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
/* Fetching the WeChat login QR performs a blocking HTTPS request, so it must run
 * on its own task with an adequate stack (mirrors the httpd task's 8192 bytes),
 * never on the Wi-Fi event task that invokes start_business_setup. */
static void main_business_setup_task(void *arg)
{
    (void)arg;
    /* Runs the full binding to completion: fetch + draw QR, poll until the user
     * confirms in WeChat, persist credentials, then switch to the default mode. */
    esp_err_t err = app_wechat_setup_run_screen_binding();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WeChat screen binding ended: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static esp_err_t main_start_business_setup(void)
{
    /* Wi-Fi may reconnect while business setup is still pending; spawn the QR
     * fetch task only once. cap_im_wechat itself ignores duplicate starts once
     * a QR session is active, but this avoids piling up short-lived tasks. */
    static volatile bool s_business_setup_started;
    app_config_t *config = NULL;
    esp_err_t err;

    if (app_runtime_state_business_ready()) {
        ESP_LOGI(TAG, "Business setup already complete; skip WeChat QR");
        return product_mode_controller_open_default();
    }

    config = calloc(1, sizeof(*config));
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = app_config_load(config);
    if (err == ESP_OK && app_runtime_state_business_ready_from_config(config)) {
        ESP_LOGI(TAG, "Business setup restored from config; skip WeChat QR");
        err = app_runtime_state_update_config(config);
        free(config);
        if (err != ESP_OK) {
            return err;
        }
        return product_mode_controller_open_default();
    }
    free(config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reload config before business setup failed: %s", esp_err_to_name(err));
    }

    if (s_business_setup_started) {
        return ESP_OK;
    }
    s_business_setup_started = true;

    BaseType_t ok = xTaskCreate(main_business_setup_task, "biz_setup", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        s_business_setup_started = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
#endif

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_fatfs(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t err;

    err = esp_vfs_fat_spiflash_mount_rw_wl(app_fatfs_base_path,
                                           APP_FATFS_PARTITION_LABEL,
                                           &mount_config,
                                           &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_vfs_fat_info(app_fatfs_base_path, &total, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to query FATFS info: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "FATFS mounted total=%u used=%u",
                 (unsigned int)total,
                 (unsigned int)(total - free_bytes));
    }

    return ESP_OK;
}

static esp_err_t init_ramfs(void)
{
    claw_ramfs_config_t config = {
        .base_path = APP_RAMFS_BASE_PATH,
        .max_files = APP_RAMFS_MAX_FILES,
        .max_bytes = APP_RAMFS_MAX_BYTES,
        .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    };
    esp_err_t err = claw_ramfs_register(&config);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount RAMFS at %s: %s", APP_RAMFS_BASE_PATH, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "RAMFS mounted at %s max_files=%u max_bytes=%u",
             APP_RAMFS_BASE_PATH,
             (unsigned int)APP_RAMFS_MAX_FILES,
             (unsigned int)APP_RAMFS_MAX_BYTES);

    return ESP_OK;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t err = ESP_OK;

    if (!timezone || timezone[0] == '\0') {
        ESP_LOGE(TAG, "Timezone is empty.");
        err = ESP_ERR_INVALID_ARG;
        goto tz_default;
    }

    if (setenv("TZ", timezone, 1) != 0) {
        ESP_LOGE(TAG, "Failed to set TZ env");
        err = ESP_FAIL;
        goto tz_default;
    }
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return err;
}

#if APP_ENABLE_MEM_LOG

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
    }
}

#endif

void app_main(void)
{
    app_config_t *config;

    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("http_reuse", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_ERROR_CHECK(app_runtime_state_init());
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(app_runtime_state_load_config());
    config = app_runtime_state_config();
    init_timezone(app_config_get_timezone(config));

    ESP_ERROR_CHECK(esp_board_manager_init());
    ESP_ERROR_CHECK(init_fatfs());
    ESP_ERROR_CHECK(init_ramfs());

    ESP_ERROR_CHECK(product_mode_controller_init(&(product_mode_controller_config_t) {
        .is_ready = app_runtime_state_business_ready,
    }));
    ESP_ERROR_CHECK(app_wechat_setup_init(&(app_wechat_setup_config_t) {
        .open_default_mode = product_mode_controller_open_default,
        .load_config = app_config_load,
        .save_config = main_save_config,
    }));
    ESP_ERROR_CHECK(app_provisioning_init(&(app_provisioning_config_t) {
        .is_business_ready = app_runtime_state_business_ready,
        .open_default_mode = product_mode_controller_open_default,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
        .start_business_setup = main_start_business_setup,
#endif
    }));

    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fatfs_base_path,
        .services = {
            .load_config = app_config_load,
            .save_config = main_save_config,
            .apply_wifi_config = app_provisioning_apply_wifi_config,
            .get_wifi_status = app_provisioning_get_wifi_status,
            .restart_device = main_restart_device,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
            .wechat_login_start = app_wechat_setup_login_start,
            .wechat_login_get_status = app_wechat_setup_login_get_status,
            .wechat_login_cancel = app_wechat_setup_login_cancel,
            .wechat_login_mark_persisted = app_wechat_setup_login_mark_persisted,
#endif
        },
    }));

    ESP_ERROR_CHECK(app_provisioning_start(config));
    ESP_ERROR_CHECK(app_product_claw_start(config,
                                           app_fatfs_base_path,
                                           !app_runtime_state_business_ready()));
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    ESP_ERROR_CHECK(http_server_webim_bind_im());
#endif

    register_wifi_command();

#if APP_ENABLE_MEM_LOG
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif
}
