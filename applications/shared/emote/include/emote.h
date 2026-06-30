/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t emote_start(void);
esp_err_t emote_set_network_status(bool sta_connected, const char *ap_ssid);

#ifdef __cplusplus
}
#endif
