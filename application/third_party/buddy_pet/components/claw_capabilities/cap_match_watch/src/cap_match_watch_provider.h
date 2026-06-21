/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "claw_cap.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_match_watch_provider_start_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size);
esp_err_t cap_match_watch_provider_stop_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size);
esp_err_t cap_match_watch_provider_status_execute(const char *input_json,
                                                  const claw_cap_call_context_t *ctx,
                                                  char *output,
                                                  size_t output_size);

#ifdef __cplusplus
}
#endif
