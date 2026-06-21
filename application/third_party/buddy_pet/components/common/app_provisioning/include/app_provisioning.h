/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "app_config.h"
#include "esp_err.h"
#include "http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool (*is_business_ready)(void);
    esp_err_t (*open_default_mode)(void);
    /* Invoked from the Wi-Fi state callback when STA connects but business setup
     * is not yet complete (e.g. WeChat binding pending). Must be non-blocking:
     * it runs on the Wi-Fi event task, so the implementation should defer any
     * network I/O (such as fetching the WeChat QR) to its own task. */
    esp_err_t (*start_business_setup)(void);
} app_provisioning_config_t;

esp_err_t app_provisioning_init(const app_provisioning_config_t *config);
esp_err_t app_provisioning_start(const app_config_t *config);
esp_err_t app_provisioning_apply_wifi_config(const app_config_t *config);
esp_err_t app_provisioning_get_wifi_status(http_server_wifi_status_t *status);
void app_provisioning_publish_match_watch_network_ready(void);
void app_provisioning_publish_match_watch_network_ready_force(const char *reason);

#ifdef __cplusplus
}
#endif
