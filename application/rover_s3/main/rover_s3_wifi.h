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

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rover_s3_wifi_state_cb_t)(bool connected, void *user_ctx);

esp_err_t rover_s3_wifi_init(void);
esp_err_t rover_s3_wifi_start(const char *ssid, const char *password);
esp_err_t rover_s3_wifi_wait_connected(uint32_t timeout_ms);
esp_err_t rover_s3_wifi_register_state_cb(rover_s3_wifi_state_cb_t cb, void *user_ctx);
bool rover_s3_wifi_is_connected(void);
const char *rover_s3_wifi_get_ip(void);

esp_err_t rover_s3_wifi_start_ap(const char *ssid);
bool rover_s3_wifi_is_ap_active(void);
esp_netif_t *rover_s3_wifi_get_ap_netif(void);

#ifdef __cplusplus
}
#endif
