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

esp_err_t app_expression_emote_start(void);
esp_err_t app_expression_emote_set_status(bool sta_connected,
                                          const char *ap_ssid,
                                          const char *sta_ip);

#ifdef __cplusplus
}
#endif
