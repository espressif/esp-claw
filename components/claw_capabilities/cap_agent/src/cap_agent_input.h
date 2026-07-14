/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_agent_input_request_store(uint32_t session_id, uint32_t request_id);

esp_err_t cap_agent_input_request_get(uint32_t session_id, uint32_t *out_request_id);

void cap_agent_input_request_clear(uint32_t session_id, uint32_t request_id);

#ifdef __cplusplus
}
#endif
