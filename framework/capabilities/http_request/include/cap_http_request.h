/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_cabi.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_http_request_register_group(claw_capability_registry_t *registry);
esp_err_t cap_http_request_set_allowlist(const char *allowlist_csv);

#ifdef __cplusplus
}
#endif
