/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "esp_mcp_engine.h"
#include "esp_mcp_mgr.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"
#include "mcp_mdns.h"

#include "cap_mcp_server.h"

static const char *TAG = "cap_mcp_srv";

static esp_mcp_t *s_mcp;
static esp_mcp_mgr_handle_t s_mgr;

static esp_err_t cap_mcp_server_register_tool(const cap_mcp_server_tool_def_t *tool_def)
{
    esp_mcp_tool_t *tool = NULL;
    size_t i = 0;

    ESP_RETURN_ON_FALSE(tool_def != NULL, ESP_ERR_INVALID_ARG, TAG, "tool def missing");
    ESP_RETURN_ON_FALSE(s_mcp != NULL, ESP_ERR_INVALID_STATE, TAG, "MCP server not initialized");
    ESP_RETURN_ON_FALSE(tool_def->name && tool_def->name[0], ESP_ERR_INVALID_ARG, TAG, "tool name missing");
    ESP_RETURN_ON_FALSE(tool_def->callback != NULL, ESP_ERR_INVALID_ARG, TAG, "tool callback missing");
    ESP_RETURN_ON_FALSE(tool_def->property_count <=
                        sizeof(tool_def->property_names) / sizeof(tool_def->property_names[0]),
                        ESP_ERR_INVALID_SIZE, TAG, "too many tool properties");

    tool = esp_mcp_tool_create(tool_def->name, tool_def->description, tool_def->callback);
    ESP_RETURN_ON_FALSE(tool != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create MCP tool");

    for (i = 0; i < tool_def->property_count; i++) {
        esp_mcp_property_t *property = NULL;
        const char *property_name = tool_def->property_names[i];

        ESP_RETURN_ON_FALSE(property_name && property_name[0],
                            ESP_ERR_INVALID_ARG, TAG, "tool property name missing");
        property = esp_mcp_property_create_with_string(property_name, "");
        ESP_RETURN_ON_FALSE(property != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create property");
        ESP_RETURN_ON_ERROR(esp_mcp_tool_add_property(tool, property), TAG, "Failed to add MCP property");
    }

    return esp_mcp_add_tool(s_mcp, tool);
}

static esp_err_t cap_mcp_server_register_discovery_service(void)
{
    mcp_mdns_config_t info = {0};
    esp_err_t err = mcp_mdns_get_config(&info);
    if (err != ESP_OK) {
        return err;
    }

    const mcp_mdns_service_info_t service = {
        .hostname = info.hostname,
        .instance_name = info.instance_name,
        .endpoint = info.endpoint,
        .port = info.server_port,
    };

    return mcp_mdns_add_service(&service);
}

esp_err_t cap_mcp_server_add_tool(const cap_mcp_server_tool_def_t *tool_defs, uint16_t tool_count)
{
    esp_err_t err = ESP_OK;
    size_t i = 0;

    if (tool_count == 0) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(tool_defs != NULL, ESP_ERR_INVALID_ARG, TAG, "tool defs missing");

    for (i = 0; i < tool_count; i++) {
        err = cap_mcp_server_register_tool(&tool_defs[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register tool %s: %s", tool_defs[i].name, esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t cap_mcp_server_init(void)
{
    if (s_mcp) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_mcp_create(&s_mcp), TAG, "Failed to create MCP engine");
    return ESP_OK;
}

esp_err_t cap_mcp_server_start(void)
{
    mcp_mdns_config_t info = {0};
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    esp_mcp_mgr_config_t config;
    esp_err_t err = ESP_OK;

    err = mcp_mdns_get_config(&info);
    if (err != ESP_OK) {
        return err;
    }

    if (info.started) {
        ESP_LOGW(TAG, "MCP server already running");
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(s_mcp != NULL, ESP_ERR_INVALID_STATE, TAG, "MCP server not initialized");

    ESP_RETURN_ON_ERROR(mcp_mdns_init(info.hostname, info.instance_name), TAG, "Failed to initialize MCP mDNS host");

    http_config.server_port = info.server_port;
    http_config.ctrl_port = info.ctrl_port;
    http_config.max_uri_handlers = 4;
    http_config.stack_size = 8192;

    uint32_t task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= http_config.stack_size) {
        task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
    http_config.task_caps = task_caps;

    config.transport = esp_mcp_transport_http_server;
    config.config = &http_config;
    config.instance = s_mcp;

    err = esp_mcp_mgr_init(config, &s_mgr);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_mcp_mgr_start(s_mgr);
    if (err != ESP_OK) {
        esp_mcp_mgr_deinit(s_mgr);
        s_mgr = 0;
        return err;
    }

    err = esp_mcp_mgr_register_endpoint(s_mgr, info.endpoint, NULL);
    if (err != ESP_OK) {
        esp_mcp_mgr_stop(s_mgr);
        esp_mcp_mgr_deinit(s_mgr);
        s_mgr = 0;
        return err;
    }

    err = cap_mcp_server_register_discovery_service();
    if (err != ESP_OK) {
        esp_mcp_mgr_stop(s_mgr);
        esp_mcp_mgr_deinit(s_mgr);
        s_mgr = 0;
        return err;
    }

    mcp_mdns_set_started(true);
    ESP_LOGI(TAG,
             "MCP server ready: http://%s.local:%u/%s (ctrl_port=%u)",
             info.hostname,
             (unsigned int)info.server_port,
             info.endpoint,
             (unsigned int)info.ctrl_port);
    return ESP_OK;
}

esp_err_t cap_mcp_server_stop(void)
{
    mcp_mdns_config_t info = {0};
    esp_err_t ret = ESP_OK;
    esp_err_t err;

    err = mcp_mdns_get_config(&info);
    if (err != ESP_OK) {
        return err;
    }

    if (!info.started) {
        return ESP_OK;
    }

    mcp_mdns_remove_service();

    if (s_mgr != 0) {
        err = esp_mcp_mgr_stop(s_mgr);
        if (err != ESP_OK && ret == ESP_OK) {
            ret = err;
        }

        err = esp_mcp_mgr_deinit(s_mgr);
        if (err != ESP_OK && ret == ESP_OK) {
            ret = err;
        }
        s_mgr = 0;
    }

    err = mcp_mdns_set_started(false);
    if (err != ESP_OK && ret == ESP_OK) {
        ret = err;
    }
    return ret;
}

esp_err_t cap_mcp_server_deinit(void)
{
    esp_err_t err = cap_mcp_server_stop();
    esp_err_t ret = err;

    if (s_mcp != NULL) {
        err = esp_mcp_destroy(s_mcp);
        s_mcp = NULL;
        if (err != ESP_OK && ret == ESP_OK) {
            ret = err;
        }
    }
    return ret;
}
