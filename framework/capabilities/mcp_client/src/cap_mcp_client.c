/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"

#include "cap_mcp_client.h"
#include "cap_mcp_client_priv.h"

static esp_err_t cap_mcp_client_group_init(void)
{
    return ESP_OK;
}

static void cap_mcp_extract_content_text(const cJSON *content,
                                         char *output,
                                         size_t output_size)
{
    const cJSON *item = NULL;
    size_t offset = 0;

    if (!cJSON_IsArray(content) || output_size == 0) {
        if (output_size > 0) {
            output[0] = '\0';
        }
        return;
    }

    cJSON_ArrayForEach(item, content) {
        cJSON *type = cJSON_GetObjectItem(item, "type");

        if (!cJSON_IsString(type)) {
            continue;
        }

        if (strcmp(type->valuestring, "text") == 0) {
            cJSON *text = cJSON_GetObjectItem(item, "text");
            if (cJSON_IsString(text) && text->valuestring) {
                size_t len = strlen(text->valuestring);
                size_t room = output_size - 1 - offset;

                if (room > 0) {
                    if (len > room) {
                        len = room;
                    }
                    memcpy(output + offset, text->valuestring, len);
                    offset += len;
                }
            }
        }

        if (offset >= output_size - 1) {
            break;
        }
    }

    output[offset] = '\0';
}

static esp_err_t cap_mcp_call_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output,
                                      size_t output_size)
{
    cJSON *result = NULL;
    const char *error_message = NULL;
    cJSON *is_error = NULL;
    esp_err_t err;

    (void)ctx;

    err = cap_mcp_call_remote_tool(input_json, &result);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: MCP request failed (%s)", esp_err_to_name(err));
        return err;
    }

    error_message = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
    if (error_message && error_message[0]) {
        snprintf(output, output_size, "Error: %s", error_message);
        cJSON_Delete(result);
        return ESP_OK;
    }

    cap_mcp_extract_content_text(cJSON_GetObjectItem(result, "content"), output, output_size);
    if (output[0] == '\0') {
        is_error = cJSON_GetObjectItem(result, "isError");
        if (cJSON_IsBool(is_error) && cJSON_IsTrue(is_error)) {
            snprintf(output, output_size, "Error: Tool returned application error");
        } else {
            snprintf(output, output_size, "(empty)");
        }
    }

    cJSON_Delete(result);
    return ESP_OK;
}

static esp_err_t cap_mcp_list_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output,
                                      size_t output_size)
{
    cJSON *result = NULL;
    const char *error_message = NULL;
    cJSON *tools_array = NULL;
    cJSON *tool = NULL;
    size_t offset = 0;
    esp_err_t err;

    (void)ctx;

    err = cap_mcp_list_remote_tools(input_json, &result);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: MCP request failed (%s)", esp_err_to_name(err));
        return err;
    }

    error_message = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
    if (error_message && error_message[0]) {
        snprintf(output, output_size, "Error: %s", error_message);
        cJSON_Delete(result);
        return ESP_OK;
    }

    tools_array = cJSON_GetObjectItem(result, "tools");
    if (cJSON_IsArray(tools_array)) {
        cJSON_ArrayForEach(tool, tools_array) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "name"));
            const char *description = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "description"));
            int written = snprintf(output + offset,
                                   output_size - offset,
                                   "- %s: %s\n",
                                   name ? name : "(no name)",
                                   description ? description : "");

            if (written < 0 || (size_t)written >= output_size - offset) {
                offset = output_size - 1;
                break;
            }
            offset += (size_t)written;
        }
    }

    cJSON *next_cursor = cJSON_GetObjectItem(result, "nextCursor");
    if (cJSON_IsString(next_cursor) && next_cursor->valuestring[0] && offset < output_size - 1) {
        offset += snprintf(output + offset,
                           output_size - offset,
                           "\n(nextCursor: %s)",
                           next_cursor->valuestring);
    }
    if (offset == 0) {
        snprintf(output, output_size, "(no tools)");
    } else if (offset >= output_size) {
        output[output_size - 1] = '\0';
    }

    cJSON_Delete(result);
    return ESP_OK;
}

static esp_err_t cap_mcp_discover_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    cJSON *root = NULL;
    cJSON *devices = NULL;
    cJSON *device = NULL;
    size_t offset = 0;
    size_t found = 0;
    esp_err_t err;

    (void)ctx;

    err = cap_mcp_discover_services(input_json, &root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: mDNS MCP discovery failed (%s)", esp_err_to_name(err));
        return err;
    }

    devices = cJSON_GetObjectItem(root, "devices");
    if (cJSON_IsArray(devices)) {
        cJSON_ArrayForEach(device, devices) {
            const char *instance = cJSON_GetStringValue(cJSON_GetObjectItem(device, "instance"));
            const char *hostname = cJSON_GetStringValue(cJSON_GetObjectItem(device, "hostname"));
            const char *ip = cJSON_GetStringValue(cJSON_GetObjectItem(device, "ip"));
            const char *endpoint = cJSON_GetStringValue(cJSON_GetObjectItem(device, "endpoint"));
            const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(device, "url"));
            cJSON *port = cJSON_GetObjectItem(device, "port");
            int written = snprintf(output + offset,
                                   output_size - offset,
                                   "instance=%s\nhostname=%s\nip=%s\nport=%u\nendpoint=%s\nurl=%s\n\n",
                                   instance ? instance : "(unknown)",
                                   hostname ? hostname : "(unknown)",
                                   ip ? ip : "(unresolved)",
                                   cJSON_IsNumber(port) ? (unsigned)port->valueint : 0,
                                   endpoint ? endpoint : MCP_MDNS_DEFAULT_ENDPOINT,
                                   url ? url : "(unknown)");

            if (written < 0 || (size_t)written >= output_size - offset) {
                offset = output_size - 1;
                break;
            }
            offset += (size_t)written;
            found++;
        }
    }

    cJSON_Delete(root);
    if (found == 0) {
        snprintf(output, output_size, "(no mcp servers discovered)");
    } else if (offset >= 2) {
        output[offset - 1] = '\0';
    }

    return ESP_OK;
}

static const claw_cap_descriptor_t s_mcp_client_descriptors[] = {
    {
        .id = "mcp_list_tools",
        .name = "mcp_list_tools",
        .family = "mcp",
        .description = "List tools from a remote MCP server.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"server_url\":{\"type\":\"string\"},\"endpoint\":{\"type\":\"string\"},\"cursor\":{\"type\":\"string\"}},\"required\":[\"server_url\"]}",
        .execute = cap_mcp_list_execute,
    },
    {
        .id = "mcp_call_tool",
        .name = "mcp_call_tool",
        .family = "mcp",
        .description = "Call a tool on a remote MCP server.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"server_url\":{\"type\":\"string\"},\"endpoint\":{\"type\":\"string\"},\"tool_name\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"}},\"required\":[\"server_url\",\"tool_name\"]}",
        .execute = cap_mcp_call_execute,
    },
    {
        .id = "mcp_discover",
        .name = "mcp_discover",
        .family = "mcp",
        .description = "Discover MCP servers advertised on the local network.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"timeout_ms\":{\"type\":\"integer\"},\"include_self\":{\"type\":\"boolean\"}}}",
        .execute = cap_mcp_discover_execute,
    },
};

static const claw_cap_group_t s_mcp_client_group = {
    .group_id = "cap_mcp_client",
    .descriptors = s_mcp_client_descriptors,
    .descriptor_count = sizeof(s_mcp_client_descriptors) / sizeof(s_mcp_client_descriptors[0]),
    .group_init = cap_mcp_client_group_init,
};

esp_err_t cap_mcp_client_register_group(void)
{
    if (claw_cap_group_exists(s_mcp_client_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_mcp_client_group);
}
