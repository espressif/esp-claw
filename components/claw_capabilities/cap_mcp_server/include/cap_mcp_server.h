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

#define CAP_MCP_SERVER_DEFAULT_HOSTNAME      "esp-claw"
#define CAP_MCP_SERVER_DEFAULT_INSTANCE      "ESP-Claw"

typedef struct {
    const char *hostname;
    const char *instance_name;
    const char *endpoint;
    uint16_t server_port;
    uint16_t ctrl_port;
    const char *auth_token;
} cap_mcp_server_config_t;

esp_err_t cap_mcp_server_register_group(void);
esp_err_t cap_mcp_server_set_config(const cap_mcp_server_config_t *config);
esp_err_t cap_mcp_server_get_config(cap_mcp_server_config_t *config, bool *started);

#ifdef __cplusplus
}
#endif
