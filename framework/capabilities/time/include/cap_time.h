/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "claw_core.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*cap_time_network_ready_fn)(void *ctx);
typedef void (*cap_time_sync_success_fn)(bool had_valid_time, void *ctx);

typedef struct {
    cap_time_network_ready_fn network_ready;
    void *network_ready_ctx;
    cap_time_sync_success_fn on_sync_success;
    void *on_sync_success_ctx;
    uint32_t disconnected_retry_ms;
    uint32_t sync_retry_ms;
} cap_time_sync_service_config_t;

esp_err_t cap_time_register_group(void);
// TODO: improve the workflow of cap_scheduler skill
esp_err_t cap_time_get_timezone(char *timezone, size_t timezone_size);
esp_err_t cap_time_get_current(char *output, size_t output_size);
esp_err_t cap_time_sync_now(char *output, size_t output_size);
bool cap_time_is_valid(void);
esp_err_t cap_time_sync_service_start(const cap_time_sync_service_config_t *config);
esp_err_t cap_time_sync_service_stop(void);

extern const claw_core_context_provider_t cap_time_context_provider;

#ifdef __cplusplus
}
#endif
