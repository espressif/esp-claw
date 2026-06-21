/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

static esp_err_t setup_wifi_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    app_config_t *config = NULL;
    cJSON *root = NULL;
    const char *ssid;
    const char *password;
    const char *validation_message = NULL;
    esp_err_t err;

    if (!ctx->services.apply_wifi_config) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Wi-Fi apply service unavailable");
    }

    err = http_server_parse_json_body(req, &root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    ssid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid"));
    password = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "wifi_password"));
    if (!ssid || ssid[0] == '\0') {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wifi_ssid is required");
    }

    config = calloc(1, sizeof(*config));
    if (!config) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    err = ctx->services.load_config(config);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        free(config);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load config");
    }

    strlcpy(config->wifi_ssid, ssid, sizeof(config->wifi_ssid));
    strlcpy(config->wifi_password, password ? password : "", sizeof(config->wifi_password));
    strlcpy(config->ap_behavior, "keep", sizeof(config->ap_behavior));

    err = app_config_validate_wifi(config, &validation_message);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        free(config);
        return httpd_resp_send_err(req,
                                   HTTPD_400_BAD_REQUEST,
                                   validation_message ? validation_message : "Invalid Wi-Fi config");
    }

    err = ctx->services.apply_wifi_config(config);
    cJSON_Delete(root);
    free(config);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to apply Wi-Fi config");
    }

    http_server_wifi_status_t status = {0};
    err = ctx->services.get_wifi_status(&status);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read Wi-Fi status");
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "wifi_connected", status.wifi_connected);
    cJSON_AddBoolToObject(resp, "ap_active", status.ap_active);
    http_server_json_add_string(resp, "ip", status.ip);
    http_server_json_add_string(resp, "ap_ip", status.ap_ip);
    http_server_json_add_string(resp, "wifi_mode", status.wifi_mode);
    http_server_json_add_string(resp, "message", "Wi-Fi config saved and connection started.");
    return http_server_send_json_response(req, resp);
}

esp_err_t http_server_register_setup_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/setup/wifi", .method = HTTP_POST, .handler = setup_wifi_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
