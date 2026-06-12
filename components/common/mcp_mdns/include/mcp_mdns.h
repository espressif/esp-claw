/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_MDNS_SERVICE_TYPE        "_mcp"
#define MCP_MDNS_SERVICE_PROTO       "_tcp"
#define MCP_MDNS_DEFAULT_TIMEOUT_MS  3000
#define MCP_MDNS_DEFAULT_ENDPOINT    "mcp"
#define MCP_MDNS_DEFAULT_HOSTNAME    "esp-claw"
#define MCP_MDNS_DEFAULT_INSTANCE    "ESP-Claw"
#define MCP_MDNS_DEFAULT_SERVER_PORT 18791
#define MCP_MDNS_DEFAULT_CTRL_PORT   18792

/**
 * @brief MCP HTTP service description advertised or discovered through mDNS.
 */
typedef struct {
    /** mDNS hostname without the ".local" suffix. Optional for advertised services. */
    const char *hostname;
    /** Human-readable mDNS instance name. */
    const char *instance_name;
    /** HTTP MCP endpoint path, for example "mcp". */
    const char *endpoint;
    /** HTTP MCP server port. */
    uint16_t port;
} mcp_mdns_service_info_t;

/**
 * @brief Options for querying MCP services on the local network.
 */
typedef struct {
    /** Query timeout in milliseconds; <= 0 uses MCP_MDNS_DEFAULT_TIMEOUT_MS. */
    int timeout_ms;
    /** Whether to include this device in the returned device list. */
    bool include_self;
    /** Hostname used to identify this device when filtering self entries. */
    const char *default_hostname;
    /** Endpoint fallback used when a discovered service has no endpoint TXT item. */
    const char *default_endpoint;
    /** Optional local service entry to append when include_self is true. */
    const mcp_mdns_service_info_t *self;
} mcp_mdns_query_config_t;

/**
 * @brief Local MCP mDNS configuration.
 *
 * This state is shared by the MCP server and discovery helpers. It stores the
 * local service identity and whether the local MCP server is currently running.
 */
typedef struct {
    /** Local mDNS hostname without the ".local" suffix. */
    const char *hostname;
    /** Local mDNS instance name. */
    const char *instance_name;
    /** Local HTTP MCP endpoint path. */
    const char *endpoint;
    /** Local HTTP MCP server port. */
    uint16_t server_port;
    /** Local HTTP server control port used by esp_http_server. */
    uint16_t ctrl_port;
    /** True after the local MCP server has started and advertised itself. */
    bool started;
} mcp_mdns_config_t;

/**
 * @brief Update local MCP mDNS configuration.
 *
 * Empty string fields and zero ports leave the current value unchanged. The
 * configuration cannot be changed while the local MCP server is marked started.
 *
 * @param config New local configuration values.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if config is NULL, or
 *         ESP_ERR_INVALID_STATE if the local service is already started.
 */
esp_err_t mcp_mdns_set_config(const mcp_mdns_config_t *config);

/**
 * @brief Read current local MCP mDNS configuration.
 *
 * Returned string pointers refer to internal storage owned by this component and
 * remain valid until the next successful `mcp_mdns_set_config()` call.
 *
 * @param config Output configuration structure.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG if config is NULL.
 */
esp_err_t mcp_mdns_get_config(mcp_mdns_config_t *config);

/**
 * @brief Mark whether the local MCP server is currently started.
 *
 * This flag is used by server lifecycle code and discovery logic; it does not
 * start or stop mDNS by itself.
 *
 * @param started True if the local MCP server is running.
 * @return ESP_OK.
 */
esp_err_t mcp_mdns_set_started(bool started);

/**
 * @brief Initialize the ESP-IDF mDNS subsystem for MCP use.
 *
 * The function is idempotent. Non-empty hostname and instance name values are
 * applied to the global mDNS host configuration.
 *
 * @param hostname Optional hostname without ".local".
 * @param instance_name Optional mDNS instance name.
 * @return ESP_OK on success, or an error from mdns initialization/configuration.
 */
esp_err_t mcp_mdns_init(const char *hostname, const char *instance_name);

/**
 * @brief Deinitialize the ESP-IDF mDNS subsystem used by MCP.
 *
 * @return ESP_OK.
 */
esp_err_t mcp_mdns_deinit(void);

/**
 * @brief Advertise one MCP HTTP service over mDNS.
 *
 * Ensures mDNS is initialized, adds the `_mcp._tcp` service, and publishes the
 * endpoint as a TXT item.
 *
 * @param service Service identity, endpoint, and port to advertise.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for missing required fields, or
 *         an error from mDNS service registration.
 */
esp_err_t mcp_mdns_add_service(const mcp_mdns_service_info_t *service);

/**
 * @brief Remove the advertised MCP HTTP service.
 *
 * @return ESP_OK on success, or an error from mDNS service removal.
 */
esp_err_t mcp_mdns_remove_service(void);

/**
 * @brief Query MCP HTTP services on the local network.
 *
 * Returns a heap-allocated JSON object containing a `count` and `devices` array.
 * Each device entry includes discovered hostname, instance, URL, port, and
 * endpoint data. Caller owns the returned JSON object and must free it with
 * `cJSON_Delete()`.
 *
 * @param config Optional query options; NULL uses defaults.
 * @param result_out Output JSON object on success. Set to NULL on entry.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if result_out is NULL,
 *         ESP_ERR_NO_MEM on allocation failure, or an error from mDNS query.
 */
esp_err_t mcp_mdns_query_devices(const mcp_mdns_query_config_t *config, cJSON **result_out);

#ifdef __cplusplus
}
#endif
