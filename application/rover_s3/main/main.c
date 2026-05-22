/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "rover_s3_settings.h"
#include "rover_s3_wifi.h"
#include "rover_s3_display.h"
#include "setup_device.h"
#include "app_rover_s3.h"
#include "wear_levelling.h"

static const char *TAG = "rover_s3_main";
static rover_s3_settings_t s_settings = {0};
static wl_handle_t s_wl = WL_INVALID_HANDLE;

#define FATFS_PARTITION_LABEL "storage"

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
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    return esp_vfs_fat_spiflash_mount_rw_wl(rover_s3_fatfs_base_path,
                                            FATFS_PARTITION_LABEL,
                                            &mount_cfg, &s_wl);
}

static void apply_timezone(const char *tz)
{
    if (!tz || !tz[0]) {
        return;
    }
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "timezone set: %s", tz);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting rover_s3");
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(rover_s3_settings_init());
    ESP_ERROR_CHECK(rover_s3_settings_load(&s_settings));
    apply_timezone(s_settings.time_timezone);

    ESP_ERROR_CHECK(rover_s3_board_init());
    rover_s3_display_init();
    rover_s3_display_set_state(ROVER_S3_DISPLAY_BOOT);
    rover_s3_display_refresh();

    ESP_ERROR_CHECK(init_fatfs());
    ESP_ERROR_CHECK(rover_s3_wifi_init());
    if (s_settings.wifi_ssid[0]) {
        esp_err_t err = rover_s3_wifi_start(s_settings.wifi_ssid, s_settings.wifi_password);
        if (err == ESP_OK && rover_s3_wifi_wait_connected(30000) != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi connection timeout; continuing offline");
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Wi-Fi SSID not configured; continuing offline");
    }

    ESP_ERROR_CHECK(app_rover_s3_start());
}
