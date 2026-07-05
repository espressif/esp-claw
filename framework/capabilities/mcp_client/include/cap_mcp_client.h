/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the MCP client capability group with the claw_cap registry.
 *
 * Registers group id `cap_mcp_client` with callable descriptors `mcp_list_tools`,
 * `mcp_call_tool`, and `mcp_discover` (remote tools/list, tools/call, and local-network
 * discovery). Safe to call repeatedly: if the group already exists, returns `ESP_OK`
 * without registering again.
 * This only registers capability metadata and callbacks; remote MCP requests are
 * made later when one of those capabilities is invoked.
 *
 * @return ESP_OK on success, or an error code from `claw_cap_register_group()`.
 */
esp_err_t cap_mcp_client_register_group(void);

#ifdef __cplusplus
}
#endif
