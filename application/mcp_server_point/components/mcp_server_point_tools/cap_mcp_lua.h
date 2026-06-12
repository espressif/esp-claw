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
 * @brief Register mcp_server_point Lua MCP tools.
 *
 * Adds MCP tools that proxy directly to cap_lua, including script listing,
 * writing, synchronous/asynchronous execution, async job inspection, and async
 * job cancellation.
 *
 * Call this after `cap_mcp_server_init()` and after app_claw has registered and
 * started the `cap_lua` capability group. It must run before
 * `cap_mcp_server_start()`.
 *
 * @return ESP_OK on success, or an esp_err_t from MCP tool registration.
 */
esp_err_t cap_mcp_lua_tools_init(void);

#ifdef __cplusplus
}
#endif
