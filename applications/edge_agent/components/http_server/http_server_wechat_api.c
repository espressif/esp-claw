/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

static esp_err_t wechat_login_start_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    cJSON *root = NULL;
    http_server_wechat_login_status_t *status = NULL;
    const char *account_id = NULL;
    bool force = false;
    esp_err_t err;

    if (req->content_len > 0) {
        esp_err_t err = http_server_parse_json_body(req, &root);
        if (err != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
        }
        account_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "account_id"));
        force = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "force"));
    }

    err = ctx->services.wechat_login_start ? ctx->services.wechat_login_start(account_id, force) : ESP_ERR_INVALID_STATE;
    if (root) {
        cJSON_Delete(root);
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start WeChat login");
    }

    status = calloc(1, sizeof(*status));
    if (!status) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    err = ctx->services.wechat_login_get_status ? ctx->services.wechat_login_get_status(status) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        free(status);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to fetch WeChat login status");
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        free(status);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    http_server_json_add_string(resp, "session_key", status->session_key);
    http_server_json_add_string(resp, "status", status->status);
    http_server_json_add_string(resp, "message", status->message);
    http_server_json_add_string(resp, "qr_data_url", status->qr_data_url);
    err = http_server_send_json_response(req, resp);
    free(status);
    return err;
}

static esp_err_t wechat_login_status_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_wechat_login_status_t *status = NULL;
    esp_err_t err;

    status = calloc(1, sizeof(*status));
    if (!status) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    err = ctx->services.wechat_login_get_status ? ctx->services.wechat_login_get_status(status) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        free(status);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read WeChat login status");
    }

    /* Credentials are NOT auto-saved here. The web frontend receives the
     * token and related fields, populates its form, and the user must
     * explicitly click Save to persist them via the /api/config endpoint. */

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        free(status);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "active", status->active);
    cJSON_AddBoolToObject(resp, "completed", status->completed);
    cJSON_AddBoolToObject(resp, "persisted", status->persisted);
    cJSON_AddBoolToObject(resp, "configured", status->configured);
    http_server_json_add_string(resp, "session_key", status->session_key);
    http_server_json_add_string(resp, "status", status->status);
    http_server_json_add_string(resp, "message", status->message);
    http_server_json_add_string(resp, "qr_data_url", status->qr_data_url);
    http_server_json_add_string(resp, "account_id", status->account_id);
    http_server_json_add_string(resp, "user_id", status->user_id);
    http_server_json_add_string(resp, "base_url", status->base_url);
    /* Return token to frontend only when login has completed so the user
     * can review it and save it manually.  Never include the token while
     * the session is still active (QR not yet scanned). */
    if (status->completed && status->token[0]) {
        http_server_json_add_string(resp, "token", status->token);
    }
    err = http_server_send_json_response(req, resp);
    free(status);
    return err;
}

static esp_err_t wechat_login_cancel_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    esp_err_t err = ctx->services.wechat_login_cancel ? ctx->services.wechat_login_cancel() : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to cancel WeChat login");
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    http_server_json_add_string(resp, "message", "已取消微信登录。");
    return http_server_send_json_response(req, resp);
}

esp_err_t http_server_register_wechat_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/wechat/login/start", .method = HTTP_POST, .handler = wechat_login_start_handler },
        { .uri = "/api/wechat/login/status", .method = HTTP_GET, .handler = wechat_login_status_handler },
        { .uri = "/api/wechat/login/cancel", .method = HTTP_POST, .handler = wechat_login_cancel_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
