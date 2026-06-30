/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_manager_state_cb_t)(bool connected, void *user_ctx);

typedef struct {
    const char *sta_ssid;
    const char *sta_password;
    const char *ap_ssid_prefix;
    const char *ap_ssid;
    const char *ap_password;
    const char *ap_behavior;
    uint8_t ap_channel;
    uint8_t ap_max_conn;
    uint32_t max_retry;
} wifi_manager_config_t;

typedef struct {
    bool sta_connected;
    bool ap_active;
    bool sta_configured;
    const char *sta_ip;
    const char *ap_ip;
    const char *ap_ssid;
    const char *mode;
} wifi_manager_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t primary;
    wifi_auth_mode_t authmode;
} wifi_manager_scan_record_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start(const wifi_manager_config_t *config);
esp_err_t wifi_manager_apply_sta_config(const wifi_manager_config_t *config);
esp_err_t wifi_manager_validate_config(const wifi_manager_config_t *config);
esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms);
esp_err_t wifi_manager_register_state_callback(wifi_manager_state_cb_t cb, void *user_ctx);
void wifi_manager_get_status(wifi_manager_status_t *status);
esp_netif_t *wifi_manager_get_ap_netif(void);
esp_err_t wifi_manager_scan_aps(wifi_manager_scan_record_t *records,
                                uint16_t max_records,
                                uint16_t *out_count);

#ifdef __cplusplus
}
#endif
