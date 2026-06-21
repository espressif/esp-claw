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

esp_err_t cap_pet_buddy_register_group(void);
esp_err_t cap_pet_buddy_set_action(const char *action, bool keep_pos);

#ifdef __cplusplus
}
#endif
