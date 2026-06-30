/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mcp_mdns.h"

#include <string.h>

typedef struct {
    char hostname[64];
    char instance_name[64];
    char endpoint[64];
    uint16_t server_port;
    uint16_t ctrl_port;
    bool started;
} mcp_mdns_local_service_state_t;

static mcp_mdns_local_service_state_t s_local_service = {
    .hostname = MCP_MDNS_DEFAULT_HOSTNAME,
    .instance_name = MCP_MDNS_DEFAULT_INSTANCE,
    .endpoint = MCP_MDNS_DEFAULT_ENDPOINT,
    .server_port = MCP_MDNS_DEFAULT_SERVER_PORT,
    .ctrl_port = MCP_MDNS_DEFAULT_CTRL_PORT,
};

esp_err_t mcp_mdns_set_config(const mcp_mdns_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_local_service.started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (config->hostname && config->hostname[0]) {
        strlcpy(s_local_service.hostname, config->hostname, sizeof(s_local_service.hostname));
    }
    if (config->instance_name && config->instance_name[0]) {
        strlcpy(s_local_service.instance_name, config->instance_name, sizeof(s_local_service.instance_name));
    }
    if (config->endpoint && config->endpoint[0]) {
        strlcpy(s_local_service.endpoint, config->endpoint, sizeof(s_local_service.endpoint));
    }
    if (config->server_port != 0) {
        s_local_service.server_port = config->server_port;
    }
    if (config->ctrl_port != 0) {
        s_local_service.ctrl_port = config->ctrl_port;
    }

    return ESP_OK;
}

esp_err_t mcp_mdns_get_config(mcp_mdns_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    config->hostname = s_local_service.hostname;
    config->instance_name = s_local_service.instance_name;
    config->endpoint = s_local_service.endpoint;
    config->server_port = s_local_service.server_port;
    config->ctrl_port = s_local_service.ctrl_port;
    config->started = s_local_service.started;
    return ESP_OK;
}

esp_err_t mcp_mdns_set_started(bool started)
{
    s_local_service.started = started;
    return ESP_OK;
}
