/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_mcp_server.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_event_publisher.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_mgr.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"
#include "mdns.h"

static const char *TAG = "cap_mcp_srv";

#define CAP_MCP_SERVER_DEFAULT_ENDPOINT      "mcp_server"
#define CAP_MCP_SERVER_DEFAULT_SERVICE_TYPE  "_mcp"
#define CAP_MCP_SERVER_DEFAULT_SERVICE_PROTO "_tcp"
#define CAP_MCP_SERVER_DEFAULT_PORT          18791
#define CAP_MCP_SERVER_DEFAULT_CTRL_PORT     18792

typedef struct {
    const char *name;
    const char *description;
    esp_mcp_value_t (*callback)(const esp_mcp_property_list_t *properties);
    const char *property_names[6];
    size_t property_count;
} cap_mcp_server_tool_def_t;

typedef struct {
    char hostname[64];
    char instance_name[64];
    char endpoint[64];
    uint16_t server_port;
    uint16_t ctrl_port;
} cap_mcp_server_runtime_config_t;

static cap_mcp_server_runtime_config_t s_config = {
    .hostname = CAP_MCP_SERVER_DEFAULT_HOSTNAME,
    .instance_name = CAP_MCP_SERVER_DEFAULT_INSTANCE,
    .endpoint = CAP_MCP_SERVER_DEFAULT_ENDPOINT,
    .server_port = CAP_MCP_SERVER_DEFAULT_PORT,
    .ctrl_port = CAP_MCP_SERVER_DEFAULT_CTRL_PORT,
};
static esp_mcp_t *s_mcp;
static esp_mcp_mgr_handle_t s_mgr;
static bool s_tools_registered;
static bool s_started;

static int cap_mcp_server_current_time_ms(void)
{
    struct timeval tv = {0};

    gettimeofday(&tv, NULL);
    return (int)((tv.tv_sec * 1000LL) + (tv.tv_usec / 1000));
}

static esp_mcp_value_t cap_mcp_server_result_json(cJSON *root)
{
    char *resp_json = NULL;
    esp_mcp_value_t result;

    if (!root) {
        return esp_mcp_value_create_bool(false);
    }

    resp_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!resp_json) {
        return esp_mcp_value_create_bool(false);
    }

    result = esp_mcp_value_create_string(resp_json);
    free(resp_json);
    return result;
}

static esp_mcp_value_t cap_mcp_server_report_state_callback(
    const esp_mcp_property_list_t *properties)
{
    const char *device_id = esp_mcp_property_list_get_property_string(properties, "device_id");
    const char *state_name = esp_mcp_property_list_get_property_string(properties, "state_name");
    const char *value = esp_mcp_property_list_get_property_string(properties, "value");
    claw_event_t event = {0};
    int timestamp_ms = 0;
    cJSON *resp = NULL;
    cJSON *payload = NULL;
    char *payload_json = NULL;

    if (!device_id || !device_id[0] || !state_name || !state_name[0] || !value) {
        ESP_LOGE(TAG, "device.report_state: missing required argument");
        return esp_mcp_value_create_bool(false);
    }

    timestamp_ms = cap_mcp_server_current_time_ms();
    ESP_LOGI(TAG,
             "state_report device_id=%s state_name=%s value=%s timestamp_ms=%d",
             device_id,
             state_name,
             value,
             timestamp_ms);

    payload = cJSON_CreateObject();
    if (!payload) {
        return esp_mcp_value_create_bool(false);
    }
    cJSON_AddStringToObject(payload, "device_id", device_id);
    cJSON_AddStringToObject(payload, "state_name", state_name);
    cJSON_AddStringToObject(payload, "value", value);
    cJSON_AddNumberToObject(payload, "timestamp_ms", timestamp_ms);
    payload_json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!payload_json) {
        return esp_mcp_value_create_bool(false);
    }

    strlcpy(event.event_id, "mcp-report-state", sizeof(event.event_id));
    strlcpy(event.source_cap, "mcp_server", sizeof(event.source_cap));
    strlcpy(event.event_type, "mcp_device_state_report", sizeof(event.event_type));
    strlcpy(event.source_channel, "mcp", sizeof(event.source_channel));
    strlcpy(event.content_type, "json", sizeof(event.content_type));
    strlcpy(event.message_id, state_name, sizeof(event.message_id));
    strlcpy(event.correlation_id, state_name, sizeof(event.correlation_id));
    event.timestamp_ms = timestamp_ms;
    event.session_policy = CLAW_SESSION_POLICY_TRIGGER;
    event.payload_json = payload_json;

    if (claw_event_router_publish(&event) != ESP_OK) {
        free(payload_json);
        return esp_mcp_value_create_bool(false);
    }
    free(payload_json);

    resp = cJSON_CreateObject();
    if (!resp) {
        return esp_mcp_value_create_bool(false);
    }

    cJSON_AddBoolToObject(resp, "accepted", true);
    cJSON_AddStringToObject(resp, "device_id", device_id);
    cJSON_AddStringToObject(resp, "state_name", state_name);
    cJSON_AddStringToObject(resp, "value", value);
    cJSON_AddNumberToObject(resp, "timestamp_ms", timestamp_ms);
    cJSON_AddStringToObject(resp, "event_type", "mcp_device_state_report");
    return cap_mcp_server_result_json(resp);
}

static esp_mcp_value_t cap_mcp_server_describe_callback(
    const esp_mcp_property_list_t *properties)
{
    cJSON *resp = NULL;
    (void)properties;

    resp = cJSON_CreateObject();
    if (!resp) {
        return esp_mcp_value_create_bool(false);
    }

    cJSON_AddStringToObject(resp, "hostname", s_config.hostname);
    cJSON_AddStringToObject(resp, "instance_name", s_config.instance_name);
    cJSON_AddStringToObject(resp, "endpoint", s_config.endpoint);
    cJSON_AddNumberToObject(resp, "server_port", s_config.server_port);
    cJSON_AddBoolToObject(resp, "started", s_started);
    return cap_mcp_server_result_json(resp);
}

static esp_mcp_value_t cap_mcp_server_emit_event_callback(
    const esp_mcp_property_list_t *properties)
{
    const char *event_type = esp_mcp_property_list_get_property_string(properties, "event_type");
    const char *text = esp_mcp_property_list_get_property_string(properties, "text");
    const char *target_channel = esp_mcp_property_list_get_property_string(properties, "target_channel");
    const char *target_endpoint = esp_mcp_property_list_get_property_string(properties, "target_endpoint");
    const char *payload_json = esp_mcp_property_list_get_property_string(properties, "payload_json");
    claw_event_t event = {0};
    cJSON *resp = NULL;

    if (!event_type || !event_type[0]) {
        return esp_mcp_value_create_bool(false);
    }

    strlcpy(event.event_id, "mcp-emit", sizeof(event.event_id));
    strlcpy(event.source_cap, "mcp_server", sizeof(event.source_cap));
    strlcpy(event.event_type, event_type, sizeof(event.event_type));
    strlcpy(event.source_channel, "mcp", sizeof(event.source_channel));
    strlcpy(event.content_type, (payload_json && payload_json[0]) ? "json" : "text",
            sizeof(event.content_type));
    strlcpy(event.target_channel, target_channel ? target_channel : "", sizeof(event.target_channel));
    strlcpy(event.target_endpoint, target_endpoint ? target_endpoint : "", sizeof(event.target_endpoint));
    strlcpy(event.message_id, event_type, sizeof(event.message_id));
    strlcpy(event.correlation_id, event_type, sizeof(event.correlation_id));
    event.timestamp_ms = cap_mcp_server_current_time_ms();
    event.session_policy = CLAW_SESSION_POLICY_TRIGGER;
    event.text = (char *)(text ? text : "");
    event.payload_json = (char *)(payload_json && payload_json[0] ? payload_json : "{}");

    if (claw_event_router_publish(&event) != ESP_OK) {
        return esp_mcp_value_create_bool(false);
    }

    resp = cJSON_CreateObject();
    if (!resp) {
        return esp_mcp_value_create_bool(false);
    }
    cJSON_AddBoolToObject(resp, "accepted", true);
    cJSON_AddStringToObject(resp, "event_type", event_type);
    cJSON_AddStringToObject(resp, "target_channel", target_channel ? target_channel : "");
    cJSON_AddStringToObject(resp, "target_endpoint", target_endpoint ? target_endpoint : "");
    return cap_mcp_server_result_json(resp);
}

static esp_err_t cap_mcp_server_register_tool(
    const cap_mcp_server_tool_def_t *tool_def)
{
    esp_mcp_tool_t *tool = NULL;
    size_t i = 0;

    ESP_RETURN_ON_FALSE(tool_def != NULL, ESP_ERR_INVALID_ARG, TAG, "tool def missing");

    tool = esp_mcp_tool_create(tool_def->name, tool_def->description, tool_def->callback);
    ESP_RETURN_ON_FALSE(tool != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create MCP tool");

    for (i = 0; i < tool_def->property_count; i++) {
        esp_mcp_property_t *property = NULL;

        property = esp_mcp_property_create_with_string(tool_def->property_names[i], "");
        ESP_RETURN_ON_FALSE(property != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create property");
        ESP_RETURN_ON_ERROR(esp_mcp_tool_add_property(tool, property),
                            TAG,
                            "Failed to add MCP property");
    }

    return esp_mcp_add_tool(s_mcp, tool);
}

static esp_err_t cap_mcp_server_register_tools(void)
{
    static const cap_mcp_server_tool_def_t s_tool_defs[] = {
        {
            .name = "device.report_state",
            .description =
            "Receive a device state update. Provide device_id, state_name, and value.",
            .callback = cap_mcp_server_report_state_callback,
            .property_names = {"device_id", "state_name", "value"},
            .property_count = 3,
        },
        {
            .name = "device.describe",
            .description = "Describe the local MCP server host state and endpoint.",
            .callback = cap_mcp_server_describe_callback,
            .property_count = 0,
        },
        {
            .name = "router.emit_event",
            .description = "Emit a standard router event into esp-claw. Provide event_type and optional text, target_channel, target_endpoint, payload_json.",
            .callback = cap_mcp_server_emit_event_callback,
            .property_names = {"event_type", "text", "target_channel", "target_endpoint", "payload_json"},
            .property_count = 5,
        },
    };
    size_t i = 0;
    esp_err_t err;

    if (s_tools_registered) {
        return ESP_OK;
    }

    for (i = 0; i < sizeof(s_tool_defs) / sizeof(s_tool_defs[0]); i++) {
        err = cap_mcp_server_register_tool(&s_tool_defs[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register tool %s: %s", s_tool_defs[i].name, esp_err_to_name(err));
            return err;
        }
    }

    s_tools_registered = true;
    return ESP_OK;
}

static esp_err_t cap_mcp_server_register_mdns_service(void)
{
    mdns_txt_item_t txt[] = {
        {"endpoint", s_config.endpoint},
    };
    esp_err_t err;

    err = mdns_service_add(s_config.instance_name,
                           CAP_MCP_SERVER_DEFAULT_SERVICE_TYPE,
                           CAP_MCP_SERVER_DEFAULT_SERVICE_PROTO,
                           s_config.server_port,
                           txt,
                           sizeof(txt) / sizeof(txt[0]));
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        if (err == ESP_OK) {
            return ESP_OK;
        }
    } else {
        return err;
    }

    err = mdns_service_port_set(CAP_MCP_SERVER_DEFAULT_SERVICE_TYPE,
                                CAP_MCP_SERVER_DEFAULT_SERVICE_PROTO,
                                s_config.server_port);
    if (err != ESP_OK) {
        return err;
    }

    return mdns_service_txt_set(CAP_MCP_SERVER_DEFAULT_SERVICE_TYPE,
                                CAP_MCP_SERVER_DEFAULT_SERVICE_PROTO,
                                txt,
                                sizeof(txt) / sizeof(txt[0]));
}

static esp_err_t cap_mcp_server_descriptor_init(void)
{
    if (s_mcp) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_mcp_create(&s_mcp), TAG, "Failed to create MCP engine");
    ESP_RETURN_ON_ERROR(cap_mcp_server_register_tools(), TAG, "Failed to register MCP tools");
    return ESP_OK;
}

static esp_err_t cap_mcp_server_descriptor_start(void)
{
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    esp_mcp_mgr_config_t config;
    esp_err_t err;

    if (s_started) {
        ESP_LOGW(TAG, "MCP server already running");
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(s_mcp != NULL, ESP_ERR_INVALID_STATE, TAG, "MCP server not initialized");

    err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_RETURN_ON_ERROR(mdns_hostname_set(s_config.hostname), TAG, "Failed to set mDNS hostname");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(s_config.instance_name),
                        TAG,
                        "Failed to set mDNS instance");

    http_config.server_port = s_config.server_port;
    http_config.ctrl_port = s_config.ctrl_port;
    http_config.max_uri_handlers = 4;
    http_config.stack_size = 8192;

    uint32_t task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= http_config.stack_size) {
        task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
#endif
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

    err = esp_mcp_mgr_register_endpoint(s_mgr, s_config.endpoint, NULL);
    if (err != ESP_OK) {
        esp_mcp_mgr_stop(s_mgr);
        esp_mcp_mgr_deinit(s_mgr);
        s_mgr = 0;
        return err;
    }

    err = cap_mcp_server_register_mdns_service();
    if (err != ESP_OK) {
        esp_mcp_mgr_stop(s_mgr);
        esp_mcp_mgr_deinit(s_mgr);
        s_mgr = 0;
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG,
             "MCP server ready: http://%s.local:%u/%s (ctrl_port=%u)",
             s_config.hostname,
             (unsigned int)s_config.server_port,
             s_config.endpoint,
             (unsigned int)s_config.ctrl_port);
    return ESP_OK;
}

static esp_err_t cap_mcp_server_descriptor_stop(void)
{
    esp_err_t ret = ESP_OK;
    esp_err_t err;

    if (!s_started) {
        return ESP_OK;
    }

    mdns_service_remove(CAP_MCP_SERVER_DEFAULT_SERVICE_TYPE,
                        CAP_MCP_SERVER_DEFAULT_SERVICE_PROTO);

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

    s_started = false;
    return ret;
}

static const claw_cap_descriptor_t s_mcp_server_descriptors[] = {
    {
        .id = "mcp_server",
        .name = "mcp_server",
        .family = "mcp",
        .description = "Lifecycle-managed local MCP server host.",
        .kind = CLAW_CAP_KIND_HYBRID,
        .cap_flags = CLAW_CAP_FLAG_SUPPORTS_LIFECYCLE,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .init = cap_mcp_server_descriptor_init,
        .start = cap_mcp_server_descriptor_start,
        .stop = cap_mcp_server_descriptor_stop,
    },
};

static const claw_cap_group_t s_mcp_server_group = {
    .group_id = "cap_mcp_server",
    .descriptors = s_mcp_server_descriptors,
    .descriptor_count = sizeof(s_mcp_server_descriptors) / sizeof(s_mcp_server_descriptors[0]),
};

esp_err_t cap_mcp_server_register_group(void)
{
    if (claw_cap_group_exists(s_mcp_server_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_mcp_server_group);
}

esp_err_t cap_mcp_server_set_config(const cap_mcp_server_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (config->hostname && config->hostname[0]) {
        strlcpy(s_config.hostname, config->hostname, sizeof(s_config.hostname));
    }
    if (config->instance_name && config->instance_name[0]) {
        strlcpy(s_config.instance_name, config->instance_name, sizeof(s_config.instance_name));
    }
    if (config->endpoint && config->endpoint[0]) {
        strlcpy(s_config.endpoint, config->endpoint, sizeof(s_config.endpoint));
    }
    if (config->server_port != 0) {
        s_config.server_port = config->server_port;
    }
    if (config->ctrl_port != 0) {
        s_config.ctrl_port = config->ctrl_port;
    }

    return ESP_OK;
}

esp_err_t cap_mcp_server_get_config(cap_mcp_server_config_t *config, bool *started)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    config->hostname = s_config.hostname;
    config->instance_name = s_config.instance_name;
    config->endpoint = s_config.endpoint;
    config->server_port = s_config.server_port;
    config->ctrl_port = s_config.ctrl_port;
    if (started) {
        *started = s_started;
    }

    return ESP_OK;
}
