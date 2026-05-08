/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*rover_demo_wifi_state_cb_t)(bool connected, void *user_ctx);

esp_err_t rover_demo_wifi_init(void);
esp_err_t rover_demo_wifi_start(const char *ssid, const char *password);
esp_err_t rover_demo_wifi_wait_connected(uint32_t timeout_ms);
esp_err_t rover_demo_wifi_register_state_cb(rover_demo_wifi_state_cb_t cb, void *user_ctx);
bool rover_demo_wifi_is_connected(void);
const char *rover_demo_wifi_get_ip(void);
