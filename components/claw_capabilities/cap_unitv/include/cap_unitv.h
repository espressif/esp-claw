/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart_port;
    int tx_gpio;
    int rx_gpio;
    int baud_rate;
    int rx_buffer_bytes;
    int default_timeout_ms;
    int capture_timeout_ms;
    int max_jpeg_bytes;
} cap_unitv_config_t;

typedef struct {
    const char *api_key;
    const char *backend_type;
    const char *model;
    const char *base_url;
    const char *auth_type;
    uint32_t timeout_ms;
    uint32_t max_response_tokens;
} cap_unitv_vision_config_t;

esp_err_t cap_unitv_init(const cap_unitv_config_t *config);
esp_err_t cap_unitv_register_group(void);
void cap_unitv_set_vision_config(const cap_unitv_vision_config_t *config);
void cap_unitv_register_cli(void);

#ifdef __cplusplus
}
#endif
