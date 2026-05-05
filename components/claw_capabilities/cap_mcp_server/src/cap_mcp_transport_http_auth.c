/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cap_mcp_transport_http_auth.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mcp_mgr.h"

static const char *TAG = "cap_mcp_http_auth";

#define CAP_MCP_HTTP_SESSION_ID_SIZE 48

typedef struct esp_mcp_http_auth_item_s {
    esp_mcp_mgr_handle_t handle;
    httpd_handle_t httpd;
    char *auth_token;
    char session_id[CAP_MCP_HTTP_SESSION_ID_SIZE];
} esp_mcp_http_auth_item_t;

static void cap_mcp_http_auth_cleanup_item(esp_mcp_http_auth_item_t *item)
{
    if (!item) {
        return;
    }

    if (item->auth_token) {
        free(item->auth_token);
        item->auth_token = NULL;
    }
}

static bool cap_mcp_http_auth_header_matches(const httpd_req_t *req,
                                             const char *header_name,
                                             const char *expected_value)
{
    char value[160] = {0};

    if (!req || !header_name || !expected_value || !expected_value[0]) {
        return false;
    }

    if (httpd_req_get_hdr_value_str((httpd_req_t *)req, header_name, value, sizeof(value)) != ESP_OK) {
        return false;
    }

    return strcmp(value, expected_value) == 0;
}

static bool cap_mcp_http_auth_is_authorized(httpd_req_t *req, const esp_mcp_http_auth_item_t *item)
{
    char bearer[192] = {0};

    if (!item || !item->auth_token || !item->auth_token[0]) {
        return true;
    }

    snprintf(bearer, sizeof(bearer), "Bearer %s", item->auth_token);
    return cap_mcp_http_auth_header_matches(req, "Authorization", bearer);
}

static void cap_mcp_http_add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization, Mcp-Session-Id");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Credentials", "false");
}

static esp_err_t cap_mcp_http_post_handler(httpd_req_t *req)
{
    esp_mcp_http_auth_item_t *mcp_http = NULL;
    uint8_t *outbuf = NULL;
    uint16_t outlen = 0;
    int total_len;
    int cur_len = 0;
    int recv_len;
    esp_err_t ret = ESP_OK;
    char *mbuf = NULL;

    ESP_RETURN_ON_FALSE(req, ESP_ERR_INVALID_ARG, TAG, "Invalid request");
    ESP_RETURN_ON_FALSE(req->user_ctx, ESP_ERR_INVALID_ARG, TAG, "Invalid user context");

    mcp_http = (esp_mcp_http_auth_item_t *)req->user_ctx;
    ESP_LOGI(TAG, "HTTP %s %s len=%d", (req->method == HTTP_OPTIONS) ? "OPTIONS" : "POST", req->uri, req->content_len);

    if (req->method == HTTP_OPTIONS) {
        ESP_LOGI(TAG, "Responding to CORS preflight");
        cap_mcp_http_add_cors_headers(req);
        httpd_resp_set_status(req, HTTPD_204);
        esp_err_t options_ret = httpd_resp_send(req, NULL, 0);
        ESP_LOGI(TAG, "CORS preflight response: %s", esp_err_to_name(options_ret));
        return options_ret;
    }

    if (!cap_mcp_http_auth_is_authorized(req, mcp_http)) {
        ESP_LOGW(TAG, "Unauthorized request to %s", req->uri);
        cap_mcp_http_add_cors_headers(req);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, NULL, 0);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Authorized request; reading body");

    total_len = req->content_len;
    if (total_len < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_ERR_INVALID_ARG;
    }

    mbuf = calloc(1, total_len + 1);
    ESP_RETURN_ON_FALSE(mbuf, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for message buffer");

    while (cur_len < total_len) {
        recv_len = httpd_req_recv(req, mbuf + cur_len, total_len - cur_len);
        if (recv_len <= 0) {
            ESP_LOGE(TAG, "Failed to receive request body: recv_len=%d cur_len=%d total_len=%d", recv_len, cur_len, total_len);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
            free(mbuf);
            return ESP_FAIL;
        }
        cur_len += recv_len;
    }
    mbuf[total_len] = '\0';
    ESP_LOGI(TAG, "Request body received (%d bytes)", total_len);

    ret = esp_mcp_mgr_req_handle(mcp_http->handle, req->uri + 1, (const uint8_t *)mbuf, total_len, &outbuf, &outlen);
    free(mbuf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MCP request failed: uri=%s err=%s", req->uri, esp_err_to_name(ret));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, esp_err_to_name(ret));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "MCP request succeeded: outlen=%u", (unsigned)outlen);
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Mcp-Session-Id", mcp_http->session_id);
    httpd_resp_set_type(req, "application/json");
    cap_mcp_http_add_cors_headers(req);
    if (outbuf && outlen > 0) {
        ESP_LOGI(TAG, "Sending JSON response");
        ret = httpd_resp_send(req, (char *)outbuf, outlen);
        esp_mcp_mgr_req_destroy_response(mcp_http->handle, outbuf);
        return (ret == ESP_OK) ? ESP_OK : ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sending empty response");
    ret = httpd_resp_send(req, NULL, 0);
    return (ret == ESP_OK) ? ESP_OK : ESP_FAIL;
}

static esp_err_t cap_mcp_http_server_init(esp_mcp_mgr_handle_t handle,
                                          esp_mcp_transport_handle_t *transport_handle)
{
    esp_mcp_http_auth_item_t *item = NULL;

    ESP_RETURN_ON_FALSE(transport_handle, ESP_ERR_INVALID_ARG, TAG, "Invalid transport handle pointer");

    item = calloc(1, sizeof(esp_mcp_http_auth_item_t));
    ESP_RETURN_ON_FALSE(item, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for HTTP item");

    item->handle = handle;
    snprintf(item->session_id, sizeof(item->session_id), "esp-claw-%p", (void *)item);
    *transport_handle = (esp_mcp_transport_handle_t)item;
    return ESP_OK;
}

static esp_err_t cap_mcp_http_server_deinit(esp_mcp_transport_handle_t handle)
{
    esp_mcp_http_auth_item_t *item = (esp_mcp_http_auth_item_t *)handle;

    ESP_RETURN_ON_FALSE(item, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    cap_mcp_http_auth_cleanup_item(item);
    item->handle = 0;
    free(item);
    return ESP_OK;
}

static esp_err_t cap_mcp_http_server_create_config(const void *config, void **config_out)
{
    const cap_mcp_http_auth_config_t *in = NULL;
    cap_mcp_http_auth_config_t *out = NULL;

    ESP_RETURN_ON_FALSE(config && config_out, ESP_ERR_INVALID_ARG, TAG, "Invalid configuration");
    in = (const cap_mcp_http_auth_config_t *)config;

    out = calloc(1, sizeof(cap_mcp_http_auth_config_t));
    ESP_RETURN_ON_FALSE(out, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for HTTP configuration");

    out->http_config = in->http_config;
    if (in->auth_token && in->auth_token[0]) {
        out->auth_token = strdup(in->auth_token);
        ESP_RETURN_ON_FALSE(out->auth_token, ESP_ERR_NO_MEM, TAG, "Failed to duplicate auth token");
    }

    *config_out = out;
    return ESP_OK;
}

static esp_err_t cap_mcp_http_server_delete_config(void *config)
{
    cap_mcp_http_auth_config_t *http_config = (cap_mcp_http_auth_config_t *)config;

    ESP_RETURN_ON_FALSE(http_config, ESP_ERR_INVALID_ARG, TAG, "Invalid configuration");
    if (http_config->auth_token) {
        free((void *)http_config->auth_token);
    }
    free(http_config);
    return ESP_OK;
}

static esp_err_t cap_mcp_http_server_start(esp_mcp_transport_handle_t handle, void *config)
{
    esp_mcp_http_auth_item_t *item = (esp_mcp_http_auth_item_t *)handle;
    cap_mcp_http_auth_config_t *http_config = (cap_mcp_http_auth_config_t *)config;

    ESP_RETURN_ON_FALSE(item && http_config, ESP_ERR_INVALID_ARG, TAG, "Invalid args");
    item->auth_token = http_config->auth_token ? strdup(http_config->auth_token) : NULL;
    if (http_config->auth_token && !item->auth_token) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = httpd_start(&item->httpd, &http_config->http_config);
    if (ret != ESP_OK) {
        cap_mcp_http_auth_cleanup_item(item);
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t cap_mcp_http_server_stop(esp_mcp_transport_handle_t handle)
{
    esp_mcp_http_auth_item_t *item = (esp_mcp_http_auth_item_t *)handle;

    ESP_RETURN_ON_FALSE(item, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    if (item->httpd) {
        esp_err_t ret = httpd_stop(item->httpd);
        ESP_RETURN_ON_ERROR(ret, TAG, "HTTP server stop failed: %s", esp_err_to_name(ret));
        item->httpd = NULL;
    }

    cap_mcp_http_auth_cleanup_item(item);
    return ESP_OK;
}

static esp_err_t cap_mcp_http_server_register_endpoint(esp_mcp_transport_handle_t handle,
                                                       const char *ep_name,
                                                       void *priv_data)
{
    esp_mcp_http_auth_item_t *item = (esp_mcp_http_auth_item_t *)handle;
    size_t ep_uri_len;
    char *ep_uri = NULL;
    httpd_uri_t config_handler;
    esp_err_t ret;

    (void)priv_data;
    ESP_RETURN_ON_FALSE(item && item->httpd, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(ep_name, ESP_ERR_INVALID_ARG, TAG, "Invalid endpoint name");

    ep_uri_len = strlen(ep_name) + 2;
    ep_uri = calloc(1, ep_uri_len);
    ESP_RETURN_ON_FALSE(ep_uri, ESP_ERR_NO_MEM, TAG, "Malloc failed for ep uri");

    snprintf(ep_uri, ep_uri_len, "/%s", ep_name);
    config_handler = (httpd_uri_t){
        .uri = ep_uri,
        .method = HTTP_POST,
        .handler = cap_mcp_http_post_handler,
        .user_ctx = item,
    };

    ret = httpd_register_uri_handler(item->httpd, &config_handler);
    free(ep_uri);
    ESP_RETURN_ON_ERROR(ret, TAG, "Uri handler register failed: %s", esp_err_to_name(ret));

    ep_uri = calloc(1, ep_uri_len);
    ESP_RETURN_ON_FALSE(ep_uri, ESP_ERR_NO_MEM, TAG, "Malloc failed for ep uri");
    snprintf(ep_uri, ep_uri_len, "/%s", ep_name);
    config_handler = (httpd_uri_t){
        .uri = ep_uri,
        .method = HTTP_OPTIONS,
        .handler = cap_mcp_http_post_handler,
        .user_ctx = item,
    };
    ret = httpd_register_uri_handler(item->httpd, &config_handler);
    free(ep_uri);
    ESP_RETURN_ON_ERROR(ret, TAG, "Options handler register failed: %s", esp_err_to_name(ret));
    return ESP_OK;
}

static esp_err_t cap_mcp_http_server_unregister_endpoint(esp_mcp_transport_handle_t handle,
                                                         const char *ep_name)
{
    esp_mcp_http_auth_item_t *item = (esp_mcp_http_auth_item_t *)handle;
    size_t ep_uri_len;
    char *ep_uri = NULL;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(item && item->httpd, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(ep_name, ESP_ERR_INVALID_ARG, TAG, "Invalid endpoint name");

    ep_uri_len = strlen(ep_name) + 2;
    ep_uri = calloc(1, ep_uri_len);
    ESP_RETURN_ON_FALSE(ep_uri, ESP_ERR_NO_MEM, TAG, "Malloc failed for ep uri");

    snprintf(ep_uri, ep_uri_len, "/%s", ep_name);
    ret = httpd_unregister_uri(item->httpd, ep_uri);
    free(ep_uri);
    ESP_RETURN_ON_ERROR(ret, TAG, "Uri handler unregister failed: %s", esp_err_to_name(ret));

    ep_uri = calloc(1, ep_uri_len);
    ESP_RETURN_ON_FALSE(ep_uri, ESP_ERR_NO_MEM, TAG, "Malloc failed for ep uri");
    snprintf(ep_uri, ep_uri_len, "/%s", ep_name);
    ret = httpd_unregister_uri(item->httpd, ep_uri);
    free(ep_uri);
    ESP_RETURN_ON_ERROR(ret, TAG, "Options handler unregister failed: %s", esp_err_to_name(ret));
    return ESP_OK;
}

const esp_mcp_transport_t cap_mcp_transport_http_server_auth = {
    .init = cap_mcp_http_server_init,
    .deinit = cap_mcp_http_server_deinit,
    .create_config = cap_mcp_http_server_create_config,
    .delete_config = cap_mcp_http_server_delete_config,
    .start = cap_mcp_http_server_start,
    .stop = cap_mcp_http_server_stop,
    .register_endpoint = cap_mcp_http_server_register_endpoint,
    .unregister_endpoint = cap_mcp_http_server_unregister_endpoint,
};