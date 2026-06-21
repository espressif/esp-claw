/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t config_runtime_show_qr(const char *title,
                                 const char *qr_payload,
                                 const char *hint,
                                 const char *status);
esp_err_t config_runtime_clear_qr(void);

#ifdef __cplusplus
}
#endif
