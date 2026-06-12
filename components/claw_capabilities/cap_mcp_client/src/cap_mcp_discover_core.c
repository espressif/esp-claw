/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>

#include "mcp_mdns.h"

static esp_err_t cap_mcp_parse_discover_options(const char *input_json,
                                                int *timeout_ms,
                                                bool *include_self)
{
    cJSON *input = NULL;

    if (!timeout_ms || !include_self) {
        return ESP_ERR_INVALID_ARG;
    }
    *timeout_ms = MCP_MDNS_DEFAULT_TIMEOUT_MS;
    *include_self = true;

    if (!input_json || !input_json[0]) {
        return ESP_OK;
    }

    input = cJSON_Parse(input_json);
    if (!input || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *timeout_item = cJSON_GetObjectItem(input, "timeout_ms");
    if (cJSON_IsNumber(timeout_item) && timeout_item->valueint > 0) {
        *timeout_ms = timeout_item->valueint;
    }

    cJSON *self_item = cJSON_GetObjectItem(input, "include_self");
    if (cJSON_IsBool(self_item)) {
        *include_self = cJSON_IsTrue(self_item);
    }

    cJSON_Delete(input);
    return ESP_OK;
}

static esp_err_t cap_mcp_get_self_mdns_service(mcp_mdns_service_info_t *service, bool *available)
{
    mcp_mdns_config_t info = {0};
    esp_err_t err;

    if (!service || !available) {
        return ESP_ERR_INVALID_ARG;
    }
    *available = false;

    err = mcp_mdns_get_config(&info);
    if (err != ESP_OK) {
        return err;
    }
    if (!info.started) {
        return ESP_OK;
    }

    service->hostname = (info.hostname && info.hostname[0]) ? info.hostname : MCP_MDNS_DEFAULT_HOSTNAME;
    service->instance_name = (info.instance_name && info.instance_name[0]) ? info.instance_name : MCP_MDNS_DEFAULT_INSTANCE;
    service->endpoint = (info.endpoint && info.endpoint[0]) ? info.endpoint : MCP_MDNS_DEFAULT_ENDPOINT;
    service->port = info.server_port;
    *available = true;
    return ESP_OK;
}

esp_err_t cap_mcp_discover_services(const char *input_json, cJSON **result_out)
{
    int timeout_ms = MCP_MDNS_DEFAULT_TIMEOUT_MS;
    bool include_self = true;
    bool self_available = false;
    mcp_mdns_service_info_t self = {0};
    mcp_mdns_query_config_t query_config = {0};
    esp_err_t err;

    if (!result_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *result_out = NULL;

    err = cap_mcp_parse_discover_options(input_json, &timeout_ms, &include_self);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_mcp_get_self_mdns_service(&self, &self_available);
    if (err != ESP_OK) {
        return err;
    }

    query_config.timeout_ms = timeout_ms;
    query_config.include_self = include_self;
    query_config.default_hostname = MCP_MDNS_DEFAULT_HOSTNAME;
    query_config.default_endpoint = MCP_MDNS_DEFAULT_ENDPOINT;
    query_config.self = self_available ? &self : NULL;
    return mcp_mdns_query_devices(&query_config, result_out);
}
