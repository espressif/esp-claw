/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool cap_agent_reply_route_supported(const char *channel, const char *chat_id);

esp_err_t cap_agent_reply_start(uint32_t session_id,
                                uint32_t request_id,
                                const char *channel,
                                const char *chat_id,
                                const char *correlation_id);

#ifdef __cplusplus
}
#endif
