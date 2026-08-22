/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "rover_s3_settings.h"
#include "rover_s3_wifi.h"
#include "rover_s3_display.h"
#include "rover_s3_httpd.h"
#include "setup_device.h"
#include "app_rover_s3.h"
#include "captive_dns.h"
#include "claw_paths.h"
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
    bool wifi_ok = false;
    if (s_settings.wifi_ssid[0]) {
        esp_err_t err = rover_s3_wifi_start(s_settings.wifi_ssid, s_settings.wifi_password);
        if (err == ESP_OK) {
            wifi_ok = (rover_s3_wifi_wait_connected(30000) == ESP_OK);
            if (!wifi_ok) {
                ESP_LOGW(TAG, "Wi-Fi connection timeout");
            }
        } else {
            ESP_LOGW(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Wi-Fi SSID not configured");
    }

    /* If no WiFi: start AP captive portal for initial setup */
    if (!wifi_ok) {
        ESP_LOGI(TAG, "Starting captive portal AP: Rover-S3-Setup");
        rover_s3_display_set_state(ROVER_S3_DISPLAY_OFFLINE);
        if (rover_s3_wifi_start_ap("Rover-S3-Setup") == ESP_OK) {
            /* Start DNS that redirects all queries to 192.168.4.1 */
            esp_netif_t *ap_netif = rover_s3_wifi_get_ap_netif();
            if (ap_netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
                    captive_dns_config_t dns_cfg = {
                        .ap_netif      = ap_netif,
                        .redirect_ip   = ip_info.ip.addr,
                        .configure_dhcp_dns = true,
                    };
                    captive_dns_start(&dns_cfg);
                }
            }
            rover_s3_httpd_start();
        }
        /* Don't call app_rover_s3_start() — no LLM/router needed in AP mode */
        return;
    }

    /* rover_s3 has a single FATFS partition (no separate read-only SYSTEM
     * image like edge_agent's dual-image boards); point both claw_paths
     * roots at it so app_claw's storage-path resolution has valid roots. */
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_DATA, rover_s3_fatfs_base_path));
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_SYSTEM, rover_s3_fatfs_base_path));

    ESP_ERROR_CHECK(app_rover_s3_start());
}
