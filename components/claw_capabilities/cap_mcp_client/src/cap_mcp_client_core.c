/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_mcp_client_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "cap_mcp_client_sse.h"

static const char *TAG = "mcp_client_core";

#define CAP_MCP_REQUEST_ID_CALL    1
#define CAP_MCP_REQUEST_ID_LIST    2
#define CAP_MCP_REQUEST_ID_INIT    3
#define CAP_MCP_RESPONSE_BUF_SIZE  (64 * 1024)
#define CAP_MCP_HTTP_TIMEOUT_MS    5000
#define CAP_MCP_SSE_RESPONSE_SIZE  4096

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} cap_mcp_buf_t;

typedef struct {
    cap_mcp_buf_t *body;
    char *session_id;
    size_t session_id_size;
} cap_mcp_http_ctx_t;

typedef struct {
    char auth_token[128];
    char session_id[128];
    bool force_initialize;
    bool initialized;
    bool use_sse;                 /* Use SSE transport instead of HTTP POST */
    int retry_count;              /* Used for one-time retry on session failure */
} cap_mcp_request_opts_t;

static cap_mcp_request_opts_t s_cached_opts = {0};

static esp_err_t cap_mcp_http_event_handler(esp_http_client_event_t *event)
{
    cap_mcp_http_ctx_t *ctx = (cap_mcp_http_ctx_t *)event->user_data;
    cap_mcp_buf_t *buf = ctx ? ctx->body : NULL;
    size_t needed;

    if (!ctx) {
        return ESP_OK;
    }

    /* Log all events for debugging */
    switch (event->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (event->header_key && event->header_value) {
            ESP_LOGD(TAG, "[HEADER] %s: %s", event->header_key, event->header_value);
            
            if (ctx->session_id && ctx->session_id_size > 0 && event->header_key) {
                const char *k = event->header_key;
                if (strcasecmp(k, "mcp-session-id") == 0 ||
                    strcasecmp(k, "mcp-session") == 0 ||
                    strcasecmp(k, "x-session-id") == 0 ||
                    strcasecmp(k, "session-id") == 0 ||
                    strcasecmp(k, "x-mcp-session") == 0) {
                    strlcpy(ctx->session_id, event->header_value, ctx->session_id_size);
                    ESP_LOGI(TAG, "[SESSION] Extracted %s: %s", k, event->header_value);
                }
            }
        }
        return ESP_OK;
    
    case HTTP_EVENT_ON_DATA:
        if (!buf) return ESP_OK;
        needed = buf->len + event->data_len;
        if (needed < buf->cap) {
            memcpy(buf->data + buf->len, event->data, event->data_len);
            buf->len += event->data_len;
            buf->data[buf->len] = '\0';
        } else {
            ESP_LOGW(TAG, "[DATA] Buffer full: need=%u cap=%u, discarding %d bytes",
                     (unsigned)needed, (unsigned)buf->cap, event->data_len);
        }
        return ESP_OK;
    
    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "[ERROR] HTTP Event Error");
        return ESP_OK;
    
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "[CONNECT] Connected");
        return ESP_OK;
    
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "[FINISH] Finished");
        if (buf && buf->data && buf->len < buf->cap) {
            buf->data[buf->len] = '\0';
        }
        return ESP_OK;
    
    default:
        ESP_LOGD(TAG, "[EVENT] Unhandled event %d", event->event_id);
        return ESP_OK;
    }
}

static void cap_mcp_build_full_url(const char *server_url,
                                   const char *endpoint,
                                   char *full_url,
                                   size_t full_url_size)
{
    size_t length = strnlen(server_url, 256);

    if (length == 0 || length >= full_url_size) {
        full_url[0] = '\0';
        return;
    }

    while (length > 0 && server_url[length - 1] == '/') {
        length--;
    }
    memcpy(full_url, server_url, length);
    full_url[length] = '\0';

    if (endpoint && endpoint[0] != '\0') {
        const char *trimmed = endpoint[0] == '/' ? endpoint + 1 : endpoint;

        if (*trimmed) {
            snprintf(full_url + length, full_url_size - length, "/%s", trimmed);
        }
    }
}

static esp_err_t cap_mcp_http_post(const char *url,
                                   const char *body,
                                   const char *auth_token,
                                   const char *session_id,
                                   cap_mcp_buf_t *buf,
                                   char *response_session_id,
                                   size_t response_session_id_size)
{
    cap_mcp_http_ctx_t ctx = {
        .body = buf,
        .session_id = response_session_id,
        .session_id_size = response_session_id_size,
    };
    esp_http_client_config_t *cfg = calloc(1, sizeof(esp_http_client_config_t));
    if (!cfg) return ESP_ERR_NO_MEM;
    cfg->url = url;
    cfg->method = HTTP_METHOD_POST;
    cfg->event_handler = cap_mcp_http_event_handler;
    cfg->user_data = &ctx;
    cfg->timeout_ms = CAP_MCP_HTTP_TIMEOUT_MS;
    cfg->buffer_size = 2048;
    cfg->crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status;

    client = esp_http_client_init(cfg);
    free(cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json, text/event-stream");
    if (auth_token && auth_token[0]) {
        char bearer[160];

        snprintf(bearer, sizeof(bearer), "Bearer %s", auth_token);
        esp_http_client_set_header(client, "Authorization", bearer);
    }
    if (session_id && session_id[0]) {
        esp_http_client_set_header(client, "Mcp-Session-Id", session_id);
    }
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    
    ESP_LOGD(TAG, "[REQ] URL=%s", url);
    ESP_LOGD(TAG, "[REQ] Body (len=%u): %s", (unsigned)strlen(body), body);
    
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);

    if (buf && buf->data && buf->len < buf->cap) {
        buf->data[buf->len] = '\0';
    }

    ESP_LOGI(TAG, "[RESP] HTTP status=%d, body_len=%u", status, (unsigned)buf->len);
    ESP_LOGD(TAG, "[RESP] Body (first 256): %.*s", 256, buf->data && buf->len > 0 ? buf->data : "");
    
    esp_http_client_cleanup(client);

    /* Accept timeout as success if we captured response data (SSE stream) */
    if ((err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_HTTP_CONNECT) &&
            buf->len > 0) {
        ESP_LOGD(TAG, "HTTP timeout but captured %u bytes", (unsigned)buf->len);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200 && status != 204) {
        ESP_LOGW(TAG, "HTTP status %d", status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t cap_mcp_parse_common_input(const char *input_json,
                                            char *server_url_buf,
                                            size_t server_url_buf_size,
                                            char *endpoint_buf,
                                            size_t endpoint_buf_size,
                                            char *auth_token_buf,
                                            size_t auth_token_buf_size,
                                            bool *force_initialize,
                                            char *cursor_buf,
                                            size_t cursor_buf_size,
                                            char *tool_name_buf,
                                            size_t tool_name_buf_size,
                                            cJSON **arguments_out,
                                            bool *use_sse_out)
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
        const char *endpoint = CAP_MCP_DEFAULT_ENDPOINT;
        cJSON *endpoint_item = cJSON_GetObjectItem(input, "endpoint");
        if (cJSON_IsString(endpoint_item) && endpoint_item->valuestring[0]) {
            endpoint = endpoint_item->valuestring;
        }
        strlcpy(endpoint_buf, endpoint, endpoint_buf_size);
    }

    if (auth_token_buf && auth_token_buf_size > 0) {
        cJSON *auth_item = cJSON_GetObjectItem(input, "auth_token");

        auth_token_buf[0] = '\0';
        if (cJSON_IsString(auth_item) && auth_item->valuestring[0]) {
            strlcpy(auth_token_buf, auth_item->valuestring, auth_token_buf_size);
        }
    }

    if (force_initialize) {
        cJSON *init_item = cJSON_GetObjectItem(input, "initialize");

        *force_initialize = cJSON_IsBool(init_item) && cJSON_IsTrue(init_item);
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

    if (use_sse_out) {
        cJSON *transport_item = cJSON_GetObjectItem(input, "transport");
        *use_sse_out = false;
        if (cJSON_IsString(transport_item) && transport_item->valuestring &&
                strcmp(transport_item->valuestring, "sse") == 0) {
            *use_sse_out = true;
        }
        /* Also auto-detect: if endpoint starts with /sse */
        if (!*use_sse_out && endpoint_buf && endpoint_buf_size > 0 &&
                strncmp(endpoint_buf, "/sse", 4) == 0) {
            *use_sse_out = true;
        }
        if (!*use_sse_out && server_url_buf && strstr(server_url_buf, "/sse")) {
            *use_sse_out = true;
        }
    }

    cJSON_Delete(input);
    return ESP_OK;
}

static esp_err_t cap_mcp_execute_json_rpc(const char *full_url,
                                          cJSON *request,
                                          const char *auth_token,
                                          const char *session_id,
                                          cJSON **response_out,
                                          char *response_session_id,
                                          size_t response_session_id_size)
{
    cap_mcp_buf_t response_buf = {0};
    char *body = NULL;
    esp_err_t err;

    *response_out = NULL;
    if (response_session_id && response_session_id_size > 0) {
        response_session_id[0] = '\0';
    }
    
    ESP_LOGD(TAG, "[RPC] URL=%s Session=%s", full_url, session_id ? session_id : "(none)");
    
    body = cJSON_PrintUnformatted(request);
    if (!body) {
        return ESP_FAIL;
    }

    response_buf.data = malloc(CAP_MCP_RESPONSE_BUF_SIZE);
    if (!response_buf.data) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    response_buf.cap = CAP_MCP_RESPONSE_BUF_SIZE;
    response_buf.data[0] = '\0';

    err = cap_mcp_http_post(full_url, body, auth_token, session_id, &response_buf, response_session_id, response_session_id_size);
    free(body);
    if (err != ESP_OK) {
        free(response_buf.data);
        return err;
    }

    if (response_buf.len == 0) {
        free(response_buf.data);
        return ESP_OK;
    }

    /* Auto-detect: SSE event stream or plain JSON */
    if (strncmp(response_buf.data, "event:", 6) == 0 ||
            strncmp(response_buf.data, "data:", 5) == 0) {
        /* Streamable HTTP: extract JSON from data: line(s) */
        ESP_LOGI(TAG, "Detected SSE response (%u bytes)", (unsigned)response_buf.len);
        
        /* Simple single-pass: find first data: line, extract its JSON */
        const char *ds = strstr(response_buf.data, "data:");
        if (ds) {
            ds += 5;
            while (*ds == ' ') ds++;
            /* Find end of this data line */
            const char *nl = strchr(ds, '\n');
            size_t json_len = nl ? (size_t)(nl - ds) : strlen(ds);
            
            /* Copy to separate buffer for parsing */
            char *json_buf = malloc(json_len + 1);
            if (json_buf) {
                memcpy(json_buf, ds, json_len);
                json_buf[json_len] = '\0';
                ESP_LOGD(TAG, "[SSE] JSON (%u bytes)", (unsigned)json_len);
                *response_out = cJSON_Parse(json_buf);
                free(json_buf);
                if (*response_out) {
                    free(response_buf.data);
                    return ESP_OK;
                }
                ESP_LOGW(TAG, "[SSE] cJSON parse failed on %u bytes", (unsigned)json_len);
            }
        }
        
        ESP_LOGW(TAG, "SSE parse failed; raw (200): %.*s",
                 200, response_buf.data);
        free(response_buf.data);
        return ESP_FAIL;
    }

    /* Plain JSON response */
    *response_out = cJSON_Parse(response_buf.data);
    free(response_buf.data);
    if (!*response_out) {
        ESP_LOGW(TAG, "JSON parse failed; raw response: %.*s",
                 (int)(response_buf.len > 256 ? 256 : response_buf.len),
                 response_buf.data);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Execute a JSON-RPC call using SSE transport.
 *
 * 1. Connects to the SSE endpoint to get the POST URL.
 * 2. Sends the JSON-RPC request via POST.
 * 3. Reads the response from the SSE stream.
 */
static esp_err_t cap_mcp_execute_json_rpc_sse(const char *server_url,
                                              const char *sse_endpoint,
                                              const char *auth_token,
                                              cJSON *request,
                                              cJSON **response_out)
{
    char *sse_full_url = NULL;
    char *post_url = NULL;
    char post_path[192];
    cap_mcp_sse_ctx_t *sse_ctx = NULL;
    char *response_buf = NULL;
    char *body = NULL;
    size_t server_len;
    esp_err_t err;

    *response_out = NULL;

    /* Build full SSE URL (heap to save stack) */
    server_len = strlen(server_url);
    while (server_len > 0 && server_url[server_len - 1] == '/') server_len--;
    const char *ep = (sse_endpoint && sse_endpoint[0] == '/') ? sse_endpoint + 1 : sse_endpoint;
    size_t ep_len = strlen(ep ? ep : "");

    sse_full_url = malloc(server_len + 1 + ep_len + 1);
    if (!sse_full_url) return ESP_ERR_NO_MEM;
    memcpy(sse_full_url, server_url, server_len);
    sse_full_url[server_len] = '/';
    memcpy(sse_full_url + server_len + 1, ep, ep_len + 1);

    /* Connect SSE and get POST path */
    err = cap_mcp_sse_connect(sse_full_url, auth_token, &sse_ctx,
                              post_path, sizeof(post_path));
    free(sse_full_url);
    if (err != ESP_OK) return err;

    /* Build full POST URL (heap) */
    if (post_path[0] == '/') {
        post_url = malloc(server_len + strlen(post_path) + 1);
        if (!post_url) { cap_mcp_sse_disconnect(sse_ctx); return ESP_ERR_NO_MEM; }
        memcpy(post_url, server_url, server_len);
        strcpy(post_url + server_len, post_path);
    } else {
        post_url = malloc(server_len + 1 + strlen(post_path) + 1);
        if (!post_url) { cap_mcp_sse_disconnect(sse_ctx); return ESP_ERR_NO_MEM; }
        memcpy(post_url, server_url, server_len);
        post_url[server_len] = '/';
        strcpy(post_url + server_len + 1, post_path);
    }

    body = cJSON_PrintUnformatted(request);
    if (!body) {
        free(post_url);
        cap_mcp_sse_disconnect(sse_ctx);
        return ESP_FAIL;
    }

    response_buf = malloc(CAP_MCP_SSE_RESPONSE_SIZE);
    if (!response_buf) {
        free(body);
        free(post_url);
        cap_mcp_sse_disconnect(sse_ctx);
        return ESP_ERR_NO_MEM;
    }

    err = cap_mcp_sse_request(sse_ctx, post_url, auth_token,
                              body, response_buf, CAP_MCP_SSE_RESPONSE_SIZE);
    free(body);
    free(post_url);
    cap_mcp_sse_disconnect(sse_ctx);

    if (err != ESP_OK) {
        free(response_buf);
        return err;
    }

    if (response_buf[0] == '\0') {
        free(response_buf);
        return ESP_OK;
    }

    *response_out = cJSON_Parse(response_buf);
    free(response_buf);
    if (!*response_out) return ESP_FAIL;

    return ESP_OK;
}

static esp_err_t cap_mcp_maybe_initialize_session(const char *full_url,
                                                  const char *auth_token,
                                                  const char *session_id,
                                                  bool force_initialize,
                                                  char *cached_session_id,
                                                  size_t cached_session_id_size)
{
    cJSON *params = NULL;
    cJSON *request = NULL;
    cJSON *response = NULL;
    char response_session_id[128] = {0};
    esp_err_t err;

    if (!force_initialize && cached_session_id && cached_session_id[0]) {
        return ESP_OK;
    }

    params = cJSON_CreateObject();
    request = cJSON_CreateObject();
    if (!params || !request) {
        cJSON_Delete(params);
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(params, "protocolVersion", "2025-03-26");
    cJSON *capabilities = cJSON_CreateObject();
    cJSON *client_info = cJSON_CreateObject();
    if (!capabilities || !client_info) {
        cJSON_Delete(params);
        cJSON_Delete(request);
        cJSON_Delete(capabilities);
        cJSON_Delete(client_info);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(params, "capabilities", capabilities);
    cJSON_AddStringToObject(client_info, "name", "esp-claw");
    cJSON_AddStringToObject(client_info, "version", "1.0.0");
    cJSON_AddItemToObject(params, "clientInfo", client_info);

    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", "initialize");
    cJSON_AddItemToObject(request, "params", params);
    cJSON_AddNumberToObject(request, "id", CAP_MCP_REQUEST_ID_INIT);

    err = cap_mcp_execute_json_rpc(full_url,
                                   request,
                                   auth_token,
                                   session_id,
                                   &response,
                                   response_session_id,
                                   sizeof(response_session_id));
    cJSON_Delete(request);
    if (err != ESP_OK) {
        return err;
    }

    if (response_session_id[0] && cached_session_id && cached_session_id_size > 0) {
        strlcpy(cached_session_id, response_session_id, cached_session_id_size);
        ESP_LOGI(TAG, "[INIT] Cached Session ID: %s", cached_session_id);
    } else {
        ESP_LOGW(TAG, "[INIT] No session ID received (response_session_id[0]=%d, has_cache=%d)",
                 response_session_id[0], cached_session_id ? 1 : 0);
        /* Clear any existing cached session to avoid reusing stale session IDs */
        if (cached_session_id && cached_session_id_size > 0) {
            cached_session_id[0] = '\0';
        }
        /* Mark global cached opts as not initialized to force re-init next time */
        s_cached_opts.initialized = false;
    }

    cJSON_Delete(response);
    return ESP_OK;
}

esp_err_t cap_mcp_list_remote_tools(const char *input_json, cJSON **result_out)
{
    char server_url_buf[256];
    char endpoint_buf[64];
    char auth_token_buf[128];
    char cursor_buf[128];
    char full_url[384];
    bool force_initialize = false;
    bool use_sse = false;
    cJSON *params = NULL;
    cJSON *request = NULL;
    cJSON *response = NULL;
    cJSON *root = NULL;
    cJSON *tools_out = NULL;
    cJSON *error_obj = NULL;
    cJSON *result = NULL;
    cJSON *tools_array = NULL;
    cJSON *tool = NULL;
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
                                     auth_token_buf,
                                     sizeof(auth_token_buf),
                                     &force_initialize,
                                     cursor_buf,
                                     sizeof(cursor_buf),
                                     NULL,
                                     0,
                                     NULL,
                                     &use_sse);
    if (err != ESP_OK) {
        return err;
    }

    /* Build JSON-RPC request */
    params = cJSON_CreateObject();
    request = cJSON_CreateObject();
    if (!params || !request) {
        cJSON_Delete(params);
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }

    if (cursor_buf[0]) {
        cJSON_AddStringToObject(params, "cursor", cursor_buf);
    }
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", "tools/list");
    cJSON_AddItemToObject(request, "params", params);
    cJSON_AddNumberToObject(request, "id", CAP_MCP_REQUEST_ID_LIST);

    if (use_sse) {
        /* SSE transport path */
        err = cap_mcp_execute_json_rpc_sse(server_url_buf,
                                           endpoint_buf,
                                           auth_token_buf,
                                           request,
                                           &response);
        cJSON_Delete(request);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        /* Standard HTTP POST path */
        cap_mcp_build_full_url(server_url_buf, endpoint_buf, full_url, sizeof(full_url));
        if (full_url[0] == '\0') {
            cJSON_Delete(request);
            return ESP_ERR_INVALID_ARG;
        }

        ESP_LOGI(TAG, "[DIAG] Before init: initialized=%d, cached_session=%s, force_init=%d",
                 s_cached_opts.initialized, s_cached_opts.session_id, force_initialize);

        if (force_initialize || !s_cached_opts.initialized || strcmp(s_cached_opts.session_id, "") == 0) {
            err = cap_mcp_maybe_initialize_session(full_url,
                                                   auth_token_buf,
                                                   s_cached_opts.session_id[0] ? s_cached_opts.session_id : NULL,
                                                   force_initialize,
                                                   s_cached_opts.session_id,
                                                   sizeof(s_cached_opts.session_id));
            if (err != ESP_OK) {
                cJSON_Delete(request);
                return err;
            }
            s_cached_opts.initialized = true;
            strlcpy(s_cached_opts.auth_token, auth_token_buf, sizeof(s_cached_opts.auth_token));
            ESP_LOGI(TAG, "[LIST] After init: cached session=%s", s_cached_opts.session_id);
        }

        ESP_LOGD(TAG, "[LIST] Using session_id: %s", s_cached_opts.session_id);
        err = cap_mcp_execute_json_rpc(full_url,
                                       request,
                                       auth_token_buf,
                                       s_cached_opts.session_id,
                                       &response,
                                       NULL,
                                       0);
        cJSON_Delete(request);
        if (err != ESP_OK) {
            return err;
        }
    }

    root = cJSON_CreateObject();
    tools_out = cJSON_CreateArray();
    if (!root || !tools_out) {
        cJSON_Delete(response);
        cJSON_Delete(root);
        cJSON_Delete(tools_out);
        return ESP_ERR_NO_MEM;
    }

    error_obj = cJSON_GetObjectItem(response, "error");
    if (cJSON_IsObject(error_obj)) {
        cJSON *message = cJSON_GetObjectItem(error_obj, "message");
        const char *msg = cJSON_IsString(message) ? message->valuestring : "Unknown MCP error";

        // Detect session-related errors and attempt a one-time recovery
        if (strstr(msg, "No session") || strstr(msg, "not found") || strstr(msg, "SESSION ERROR") || strstr(msg, "No session ID")) {
            ESP_LOGW(TAG, "[SESSION] Detected session error: %s", msg);
            // Clear cache and mark uninitialized
            s_cached_opts.session_id[0] = '\0';
            s_cached_opts.initialized = false;

            // Only retry once using retry_count to avoid recursion loop
            if (s_cached_opts.retry_count == 0) {
                s_cached_opts.retry_count = 1;
                cJSON_Delete(response);
                cJSON_Delete(root);
                esp_err_t retry_err = cap_mcp_list_remote_tools(input_json, result_out);
                s_cached_opts.retry_count = 0;
                return retry_err;
            }
        }

        cJSON_AddStringToObject(root,
                                "error_message",
                                msg);
        cJSON_AddItemToObject(root, "tools", tools_out);
        cJSON_Delete(response);
        *result_out = root;
        return ESP_OK;
    }

    result = cJSON_GetObjectItem(response, "result");
    if (!cJSON_IsObject(result)) {
        cJSON_Delete(response);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    tools_array = cJSON_GetObjectItem(result, "tools");
    if (cJSON_IsArray(tools_array)) {
        cJSON_ArrayForEach(tool, tools_array) {
            cJSON *duplicate = cJSON_Duplicate(tool, 1);

            if (!duplicate) {
                cJSON_Delete(response);
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddItemToArray(tools_out, duplicate);
        }
    }
    cJSON_AddItemToObject(root, "tools", tools_out);

    cJSON *next_cursor = cJSON_GetObjectItem(result, "nextCursor");
    if (cJSON_IsString(next_cursor) && next_cursor->valuestring[0]) {
        cJSON_AddStringToObject(root, "nextCursor", next_cursor->valuestring);
    }

    cJSON_Delete(response);
    *result_out = root;
    return ESP_OK;
}

esp_err_t cap_mcp_call_remote_tool(const char *input_json, cJSON **result_out)
{
    char server_url_buf[256];
    char endpoint_buf[64];
    char auth_token_buf[128];
    char tool_name_buf[128];
    char full_url[384];
    bool force_initialize = false;
    bool use_sse = false;
    cJSON *arguments = NULL;
    cJSON *params = NULL;
    cJSON *request = NULL;
    cJSON *response = NULL;
    cJSON *root = NULL;
    cJSON *error_obj = NULL;
    cJSON *result = NULL;
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
                                     auth_token_buf,
                                     sizeof(auth_token_buf),
                                     &force_initialize,
                                     NULL,
                                     0,
                                     tool_name_buf,
                                     sizeof(tool_name_buf),
                                     &arguments,
                                     &use_sse);
    if (err != ESP_OK) {
        cJSON_Delete(arguments);
        return err;
    }

    /* Build JSON-RPC request */
    params = cJSON_CreateObject();
    request = cJSON_CreateObject();
    if (!params || !request) {
        cJSON_Delete(arguments);
        cJSON_Delete(params);
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(params, "name", tool_name_buf);
    cJSON_AddItemToObject(params, "arguments", arguments);
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", "tools/call");
    cJSON_AddItemToObject(request, "params", params);
    cJSON_AddNumberToObject(request, "id", CAP_MCP_REQUEST_ID_CALL);

    if (use_sse) {
        /* SSE transport path */
        err = cap_mcp_execute_json_rpc_sse(server_url_buf,
                                           endpoint_buf,
                                           auth_token_buf,
                                           request,
                                           &response);
        cJSON_Delete(request);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        /* Standard HTTP POST path */
        cap_mcp_build_full_url(server_url_buf, endpoint_buf, full_url, sizeof(full_url));
        if (full_url[0] == '\0') {
            cJSON_Delete(request);
            return ESP_ERR_INVALID_ARG;
        }

        if (force_initialize || !s_cached_opts.initialized || strcmp(s_cached_opts.session_id, "") == 0) {
            err = cap_mcp_maybe_initialize_session(full_url,
                                                   auth_token_buf,
                                                   s_cached_opts.session_id[0] ? s_cached_opts.session_id : NULL,
                                                   force_initialize,
                                                   s_cached_opts.session_id,
                                                   sizeof(s_cached_opts.session_id));
            if (err != ESP_OK) {
                cJSON_Delete(request);
                return err;
            }
            s_cached_opts.initialized = true;
            strlcpy(s_cached_opts.auth_token, auth_token_buf, sizeof(s_cached_opts.auth_token));
        }

        err = cap_mcp_execute_json_rpc(full_url,
                                       request,
                                       auth_token_buf,
                                       s_cached_opts.session_id,
                                       &response,
                                       NULL,
                                       0);
        cJSON_Delete(request);
        if (err != ESP_OK) {
            return err;
        }
    }

    root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(response);
        return ESP_ERR_NO_MEM;
    }

    error_obj = cJSON_GetObjectItem(response, "error");
    if (cJSON_IsObject(error_obj)) {
        cJSON *message = cJSON_GetObjectItem(error_obj, "message");

        cJSON_AddStringToObject(root,
                                "error_message",
                                cJSON_IsString(message) ? message->valuestring : "Unknown MCP error");
        cJSON_Delete(response);
        *result_out = root;
        return ESP_OK;
    }

    result = cJSON_GetObjectItem(response, "result");
    if (!cJSON_IsObject(result)) {
        cJSON_Delete(response);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *content = cJSON_GetObjectItem(result, "content");
    cJSON *is_error = cJSON_GetObjectItem(result, "isError");
    cJSON_AddItemToObject(root, "content", content ? cJSON_Duplicate(content, 1) : cJSON_CreateArray());
    if (cJSON_IsBool(is_error)) {
        cJSON_AddBoolToObject(root, "isError", cJSON_IsTrue(is_error));
    }

    cJSON_Delete(response);
    *result_out = root;
    return ESP_OK;
}
