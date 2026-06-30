/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Minimal blocking Wi-Fi STA helper for the on-device network tests. */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up Wi-Fi in station mode and block until connected (with an IP) or the
 * attempt fails. Initializes NVS / netif / the default event loop on first call.
 * Returns ESP_OK once an IP is acquired.
 */
esp_err_t test_wifi_connect(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
