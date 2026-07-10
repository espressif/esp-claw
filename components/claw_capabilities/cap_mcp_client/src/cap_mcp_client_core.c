/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_mcp_mgr.h"

#include "cap_mcp_client_priv.h"

#define CAP_MCP_HTTP_TIMEOUT_MS    20000

typedef struct {
    cJSON *result;
    esp_err_t err;
} cap_mcp_response_ctx_t;

static esp_err_t cap_mcp_parse_common_input(const char *input_json,
                                            char *server_url_buf,
                                            size_t server_url_buf_size,
                                            char *endpoint_buf,
                                            size_t endpoint_buf_size,
                                            char *cursor_buf,
                                            size_t cursor_buf_size,
                                            char *tool_name_buf,
                                            size_t tool_name_buf_size,
                                            cJSON **arguments_out)
{
    cJSON *input = cJSON_Parse(input_json);

    if (!input || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *server_url_item = cJSON_GetObjectItem(input, "server_url");
    if (!cJSON_IsString(server_url_item) || !server_url_item->valuestring[0]) {
        cJSON_Delete(input);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(server_url_buf, server_url_item->valuestring, server_url_buf_size);

    if (endpoint_buf && endpoint_buf_size > 0) {
        const char *endpoint = MCP_MDNS_DEFAULT_ENDPOINT;
        cJSON *endpoint_item = cJSON_GetObjectItem(input, "endpoint");
        if (cJSON_IsString(endpoint_item) && endpoint_item->valuestring[0]) {
            endpoint = endpoint_item->valuestring;
        }
        strlcpy(endpoint_buf, endpoint, endpoint_buf_size);
    }

    if (cursor_buf && cursor_buf_size > 0) {
        cJSON *cursor_item = cJSON_GetObjectItem(input, "cursor");

        cursor_buf[0] = '\0';
        if (cJSON_IsString(cursor_item) && cursor_item->valuestring[0]) {
            strlcpy(cursor_buf, cursor_item->valuestring, cursor_buf_size);
        }
    }

    if (tool_name_buf && tool_name_buf_size > 0) {
        cJSON *tool_name_item = cJSON_GetObjectItem(input, "tool_name");
        if (!cJSON_IsString(tool_name_item) || !tool_name_item->valuestring[0]) {
            cJSON_Delete(input);
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(tool_name_buf, tool_name_item->valuestring, tool_name_buf_size);
    }

    if (arguments_out) {
        cJSON *arguments = cJSON_GetObjectItem(input, "arguments");

        if (!arguments || !cJSON_IsObject(arguments)) {
            *arguments_out = cJSON_CreateObject();
        } else {
            *arguments_out = cJSON_Duplicate(arguments, 1);
        }

        if (!*arguments_out) {
            cJSON_Delete(input);
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON_Delete(input);
    return ESP_OK;
}

static esp_err_t cap_mcp_mgr_response_cb(int error_code,
                                         const char *ep_name,
                                         const char *resp_json,
                                         void *user_ctx,
                                         uint32_t jsonrpc_request_id)
{
    cap_mcp_response_ctx_t *ctx = (cap_mcp_response_ctx_t *)user_ctx;

    (void)ep_name;
    (void)jsonrpc_request_id;

    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(ctx->result);
    ctx->result = cJSON_CreateObject();
    if (!ctx->result) {
        ctx->err = ESP_ERR_NO_MEM;
        return ctx->err;
    }

    if (error_code == 0 || error_code == 1) {
        if (resp_json && resp_json[0]) {
            cJSON *parsed = cJSON_Parse(resp_json);

            if (!cJSON_IsObject(parsed)) {
                cJSON_Delete(parsed);
                cJSON_AddStringToObject(ctx->result, "error_message", "Invalid MCP response");
                ctx->err = ESP_FAIL;
                return ctx->err;
            }
            cJSON_Delete(ctx->result);
            ctx->result = parsed;
        }
        ctx->err = ESP_OK;
        return ESP_OK;
    }

    if (error_code < 0) {
        cJSON_AddStringToObject(ctx->result,
                                "error_message",
                                (resp_json && resp_json[0]) ? resp_json : "Unknown MCP protocol error");
        ctx->err = ESP_OK;
        return ESP_OK;
    }

    cJSON_AddStringToObject(ctx->result, "error_message", esp_err_to_name(error_code));
    ctx->err = ESP_OK;
    return ESP_OK;
}

static esp_err_t cap_mcp_init_remote_session(esp_mcp_mgr_handle_t mgr, const char *endpoint)
{
    cap_mcp_response_ctx_t ctx = {0};
    esp_mcp_mgr_req_t req = {
        .ep_name = endpoint,
        .cb = cap_mcp_mgr_response_cb,
        .user_ctx = &ctx,
        .u.init = {
            .name = "esp-claw",
            .version = "1.0.0",
            .title = "ESP-Claw",
        },
    };
    esp_err_t err = esp_mcp_mgr_post_info_init(mgr, &req);

    if (err == ESP_OK) {
        err = ctx.err;
    }
    cJSON_Delete(ctx.result);
    return err;
}

static esp_err_t cap_mcp_mgr_create(const char *server_url,
                                    const char *endpoint,
                                    esp_mcp_mgr_handle_t *mgr_out)
{
    esp_err_t err;
    esp_http_client_config_t http_config = {
        .url = server_url,
        .timeout_ms = CAP_MCP_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .keep_alive_enable = true,
    };
    esp_mcp_mgr_config_t mgr_config = {
        .transport = esp_mcp_transport_http_client,
        .config = &http_config,
    };

    if (!server_url || !server_url[0] || !endpoint || !endpoint[0] || !mgr_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *mgr_out = 0;

    err = esp_mcp_mgr_init(mgr_config, mgr_out);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_mcp_mgr_start(*mgr_out);
    if (err != ESP_OK) {
        esp_mcp_mgr_deinit(*mgr_out);
        *mgr_out = 0;
        return err;
    }

    err = esp_mcp_mgr_register_endpoint(*mgr_out, endpoint, NULL);
    if (err != ESP_OK) {
        esp_mcp_mgr_stop(*mgr_out);
        esp_mcp_mgr_deinit(*mgr_out);
        *mgr_out = 0;
        return err;
    }

    err = cap_mcp_init_remote_session(*mgr_out, endpoint);
    if (err != ESP_OK) {
        esp_mcp_mgr_stop(*mgr_out);
        esp_mcp_mgr_deinit(*mgr_out);
        *mgr_out = 0;
    }
    return err;
}

static void cap_mcp_mgr_destroy(esp_mcp_mgr_handle_t mgr)
{
    if (mgr != 0) {
        esp_mcp_mgr_stop(mgr);
        esp_mcp_mgr_deinit(mgr);
    }
}

static esp_err_t cap_mcp_capture_result(cap_mcp_response_ctx_t *ctx, cJSON **result_out)
{
    if (!ctx || !result_out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->err != ESP_OK) {
        cJSON_Delete(ctx->result);
        ctx->result = NULL;
        return ctx->err;
    }
    if (!ctx->result) {
        return ESP_FAIL;
    }

    *result_out = ctx->result;
    ctx->result = NULL;
    return ESP_OK;
}

esp_err_t cap_mcp_list_remote_tools(const char *input_json, cJSON **result_out)
{
    char server_url_buf[256];
    char endpoint_buf[64];
    char cursor_buf[128];
    esp_mcp_mgr_handle_t mgr = 0;
    cap_mcp_response_ctx_t ctx = {0};
    esp_mcp_mgr_req_t req = {0};
    esp_err_t err;

    if (!input_json || !result_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *result_out = NULL;

    err = cap_mcp_parse_common_input(input_json,
                                     server_url_buf,
                                     sizeof(server_url_buf),
                                     endpoint_buf,
                                     sizeof(endpoint_buf),
                                     cursor_buf,
                                     sizeof(cursor_buf),
                                     NULL,
                                     0,
                                     NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_mcp_mgr_create(server_url_buf, endpoint_buf, &mgr);
    if (err != ESP_OK) {
        return err;
    }

    req.ep_name = endpoint_buf;
    req.cb = cap_mcp_mgr_response_cb;
    req.user_ctx = &ctx;
    req.u.list.cursor = cursor_buf[0] ? cursor_buf : NULL;
    req.u.list.limit = -1;

    err = esp_mcp_mgr_post_tools_list(mgr, &req);
    cap_mcp_mgr_destroy(mgr);
    if (err == ESP_OK) {
        err = cap_mcp_capture_result(&ctx, result_out);
    }
    cJSON_Delete(ctx.result);
    return err;
}

esp_err_t cap_mcp_call_remote_tool(const char *input_json, cJSON **result_out)
{
    char server_url_buf[256];
    char endpoint_buf[64];
    char tool_name_buf[128];
    cJSON *arguments = NULL;
    char *arguments_json = NULL;
    esp_mcp_mgr_handle_t mgr = 0;
    cap_mcp_response_ctx_t ctx = {0};
    esp_mcp_mgr_req_t req = {0};
    esp_err_t err;

    if (!input_json || !result_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *result_out = NULL;

    err = cap_mcp_parse_common_input(input_json,
                                     server_url_buf,
                                     sizeof(server_url_buf),
                                     endpoint_buf,
                                     sizeof(endpoint_buf),
                                     NULL,
                                     0,
                                     tool_name_buf,
                                     sizeof(tool_name_buf),
                                     &arguments);
    if (err != ESP_OK) {
        cJSON_Delete(arguments);
        return err;
    }

    arguments_json = cJSON_PrintUnformatted(arguments);
    cJSON_Delete(arguments);
    if (!arguments_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_mcp_mgr_create(server_url_buf, endpoint_buf, &mgr);
    if (err != ESP_OK) {
        cJSON_free(arguments_json);
        return err;
    }

    req.ep_name = endpoint_buf;
    req.cb = cap_mcp_mgr_response_cb;
    req.user_ctx = &ctx;
    req.u.call.tool_name = tool_name_buf;
    req.u.call.args_json = arguments_json;

    err = esp_mcp_mgr_post_tools_call(mgr, &req);
    cap_mcp_mgr_destroy(mgr);
    cJSON_free(arguments_json);
    if (err == ESP_OK) {
        err = cap_mcp_capture_result(&ctx, result_out);
    }
    cJSON_Delete(ctx.result);
    return err;
}
