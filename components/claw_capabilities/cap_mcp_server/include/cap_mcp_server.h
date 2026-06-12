/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_property.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Definition of one MCP tool exposed by cap_mcp_server.
 *
 * The server creates an MCP tool from this description and registers string
 * properties named in `property_names`. Tool callbacks receive the property
 * list supplied by the MCP client and must return an MCP value response.
 */
typedef struct {
    /** MCP tool name, for example "lua.list_scripts". Must remain valid while registering. */
    const char *name;
    /** Human-readable tool description advertised to MCP clients. */
    const char *description;
    /** Callback invoked when the MCP client calls this tool. */
    esp_mcp_value_t (*callback)(const esp_mcp_property_list_t *properties);
    /** Names of string properties accepted by this tool. */
    const char *property_names[6];
    /** Number of valid entries in `property_names`; maximum is 6. */
    size_t property_count;
} cap_mcp_server_tool_def_t;

/**
 * @brief Initialize the MCP server.
 *
 * Creates the underlying MCP engine instance. This function is idempotent; if
 * the MCP engine already exists it returns ESP_OK.
 *
 * Call this before `cap_mcp_server_add_tool()` or `cap_mcp_server_start()`.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM or another error from `esp_mcp_create()`.
 */
esp_err_t cap_mcp_server_init(void);

/**
 * @brief Start the MCP server.
 *
 * Starts the HTTP MCP transport using the current `mcp_mdns` configuration,
 * registers the MCP endpoint, and advertises the service over mDNS. If the
 * server is already marked as started, this function returns ESP_OK.
 *
 * Call this after `cap_mcp_server_init()` and after all desired tools have been
 * registered with `cap_mcp_server_add_tool()`.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized, or an
 *         error from mDNS / MCP manager startup.
 */
esp_err_t cap_mcp_server_start(void);

/**
 * @brief Stop the MCP server.
 *
 * Removes the mDNS service, stops and deinitializes the MCP manager, and marks
 * the server as stopped. If the server is already stopped, this function returns
 * ESP_OK.
 *
 * @return ESP_OK on success, or the first error reported by mDNS or MCP manager
 *         shutdown.
 */
esp_err_t cap_mcp_server_stop(void);

/**
 * @brief Deinitialize the MCP server.
 *
 * Stops the server if needed and destroys the underlying MCP engine instance.
 * Registered tool definitions are released with the MCP engine.
 *
 * @return ESP_OK on success, or the first error reported by stop/destroy.
 */
esp_err_t cap_mcp_server_deinit(void);

/**
 * @brief Register MCP tools with the server.
 *
 * Each definition is converted to an `esp_mcp_tool_t` and added to the MCP
 * engine. The server must be initialized before calling this function.
 *
 * @param tool_defs Array of tool definitions to register.
 * @param tool_count Number of entries in `tool_defs`.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid definition,
 *         ESP_ERR_NO_MEM on allocation failure, or an error from MCP tool
 *         registration.
 */
esp_err_t cap_mcp_server_add_tool(const cap_mcp_server_tool_def_t *tool_defs, uint16_t tool_count);

#ifdef __cplusplus
}
#endif
