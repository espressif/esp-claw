/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_http_server.h"
#include "esp_mcp_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    httpd_config_t http_config;
    const char *auth_token;
} cap_mcp_http_auth_config_t;

extern const esp_mcp_transport_t cap_mcp_transport_http_server_auth;

#ifdef __cplusplus
}
#endif