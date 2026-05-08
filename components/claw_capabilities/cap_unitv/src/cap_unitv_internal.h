/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>

#include "cap_unitv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    cap_unitv_config_t cfg;
    cap_unitv_vision_config_t vision_cfg;
    char vision_api_key[192];
    char vision_model[64];
    char vision_base_url[128];
    char vision_backend_type[32];
    char vision_auth_type[16];
    bool vision_configured;
    SemaphoreHandle_t uart_mutex;
    atomic_uint_fast32_t req_seq;
    atomic_bool available;
    bool initialized;
} cap_unitv_state_t;

extern cap_unitv_state_t g_cap_unitv;

esp_err_t cap_unitv_uart_cmd(const char *cmd, const char *args_json,
                             char *resp, size_t resp_size, int timeout_ms);
esp_err_t cap_unitv_uart_capture_jpeg(int quality, uint8_t **jpeg_out, size_t *jpeg_size_out);
esp_err_t cap_unitv_vision_call(const char *question, const uint8_t *jpeg, size_t jpeg_size,
                                char *resp, size_t resp_size);

#ifdef __cplusplus
}
#endif
