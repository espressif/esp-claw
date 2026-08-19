/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "http_server_priv.h"

static esp_err_t openai_send_status(
    httpd_req_t *req,
    const http_server_openai_login_status_t *status)
{
    cJSON *resp = cJSON_CreateObject();

    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "active", status->active);
    cJSON_AddBoolToObject(resp, "completed", status->completed);

    http_server_json_add_string(resp,
                                "status",
                                status->status);

    http_server_json_add_string(resp,
                                "message",
                                status->message);

    http_server_json_add_string(resp,
                                "user_code",
                                status->user_code);

    http_server_json_add_string(resp,
                                "verification_url",
                                status->verification_url);

    cJSON_AddNumberToObject(resp,
                            "interval",
                            status->interval);

    return http_server_send_json_response(req, resp);
}

static esp_err_t openai_login_start_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_openai_login_status_t status = {0};

    esp_err_t err =
        ctx->services.openai_login_start
            ? ctx->services.openai_login_start()
            : ESP_ERR_INVALID_STATE;

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to start OpenAI login");
    }

    err =
        ctx->services.openai_login_get_status
            ? ctx->services.openai_login_get_status(&status)
            : ESP_ERR_INVALID_STATE;

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to read OpenAI login status");
    }

    return openai_send_status(req, &status);
}

static esp_err_t openai_login_status_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_openai_login_status_t status = {0};

    esp_err_t err =
        ctx->services.openai_login_get_status
            ? ctx->services.openai_login_get_status(&status)
            : ESP_ERR_INVALID_STATE;

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to read OpenAI login status");
    }

    return openai_send_status(req, &status);
}

static esp_err_t openai_login_cancel_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_openai_login_status_t status = {0};

    esp_err_t err =
        ctx->services.openai_login_cancel
            ? ctx->services.openai_login_cancel()
            : ESP_ERR_INVALID_STATE;

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to cancel OpenAI login");
    }

    err =
        ctx->services.openai_login_get_status
            ? ctx->services.openai_login_get_status(&status)
            : ESP_ERR_INVALID_STATE;

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to read OpenAI login status");
    }

    return openai_send_status(req, &status);
}

esp_err_t http_server_register_openai_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        {
            .uri = "/api/openai/login/start",
            .method = HTTP_POST,
            .handler = openai_login_start_handler
        },
        {
            .uri = "/api/openai/login/status",
            .method = HTTP_GET,
            .handler = openai_login_status_handler
        },
        {
            .uri = "/api/openai/login/cancel",
            .method = HTTP_POST,
            .handler = openai_login_cancel_handler
        },
    };

    for (size_t i = 0;
            i < sizeof(handlers) / sizeof(handlers[0]);
            ++i) {
        esp_err_t err =
            httpd_register_uri_handler(server, &handlers[i]);

        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}