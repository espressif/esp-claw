/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "cJSON.h"
#include "esp_err.h"

#define CAP_MCP_DEFAULT_ENDPOINT      "mcp"
#define CAP_MCP_MDNS_SERVICE_TYPE     "_mcp"
#define CAP_MCP_MDNS_SERVICE_PROTO    "_tcp"
#define CAP_MCP_DISCOVER_TIMEOUT_MS   3000

esp_err_t cap_mcp_list_remote_tools(const char *input_json, cJSON **result_out);
esp_err_t cap_mcp_call_remote_tool(const char *input_json, cJSON **result_out);
esp_err_t cap_mcp_discover_services(const char *input_json, cJSON **result_out);
