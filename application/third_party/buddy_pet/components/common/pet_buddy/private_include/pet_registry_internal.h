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

esp_err_t pet_registry_select(const char *pet_id);
esp_err_t pet_registry_clear_selected(void);

#ifdef __cplusplus
}
#endif
