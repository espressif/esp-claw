/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <string.h>

static esp_err_t status_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_wifi_status_t status = {0};
    http_server_runtime_status_t runtime = {0};
    esp_err_t err = ctx->services.get_wifi_status(&status);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read Wi-Fi status");
    }
    if (ctx->services.get_runtime_status) {
        err = ctx->services.get_runtime_status(&runtime);
        if (err != ESP_OK) {
            memset(&runtime, 0, sizeof(runtime));
            runtime.router_available = false;
            strlcpy(runtime.router_state, "unknown", sizeof(runtime.router_state));
            strlcpy(runtime.router_reason, "runtime status unavailable", sizeof(runtime.router_reason));
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *runtime_obj = NULL;
    cJSON *router_obj = NULL;
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "wifi_connected", status.wifi_connected);
    http_server_json_add_string(root, "ip", status.ip);
    http_server_json_add_string(root, "storage_base_path", ctx->storage_base_path);
    cJSON_AddBoolToObject(root, "ap_active", status.ap_active);
    http_server_json_add_string(root, "ap_ssid", status.ap_ssid);
    http_server_json_add_string(root, "ap_ip", status.ap_ip);
    http_server_json_add_string(root, "wifi_mode", status.wifi_mode);

    runtime_obj = cJSON_CreateObject();
    router_obj = cJSON_CreateObject();
    if (!runtime_obj || !router_obj) {
        cJSON_Delete(root);
        cJSON_Delete(runtime_obj);
        cJSON_Delete(router_obj);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(runtime_obj, "safe_mode", runtime.safe_mode);
    http_server_json_add_string(runtime_obj, "safe_mode_reason", runtime.safe_mode_reason);
    http_server_json_add_string(runtime_obj, "reset_reason", runtime.reset_reason);
    cJSON_AddBoolToObject(router_obj, "available", runtime.router_available);
    http_server_json_add_string(router_obj, "state", runtime.router_state);
    http_server_json_add_string(router_obj, "reason", runtime.router_reason);
    cJSON_AddNumberToObject(router_obj, "event_queue_depth", runtime.router_event_queue_depth);
    cJSON_AddNumberToObject(router_obj, "action_queue_depth", runtime.router_action_queue_depth);
    cJSON_AddNumberToObject(router_obj, "router_stack_hwm_bytes", runtime.router_stack_hwm_bytes);
    cJSON_AddNumberToObject(router_obj, "action_stack_hwm_bytes", runtime.router_action_stack_hwm_bytes);
    cJSON_AddNumberToObject(router_obj, "failed_actions", runtime.router_failed_actions);
    cJSON_AddNumberToObject(router_obj, "dropped_events", runtime.router_dropped_events);
    cJSON_AddNumberToObject(router_obj, "last_error", runtime.router_last_error);
    cJSON_AddItemToObject(runtime_obj, "router", router_obj);
    cJSON_AddItemToObject(root, "runtime", runtime_obj);
    return http_server_send_json_response(req, root);
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    esp_err_t err = ctx->services.restart_device ? ctx->services.restart_device() : ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to restart device");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "message", "device restart scheduled");
    return http_server_send_json_response(req, root);
}

static esp_err_t clear_safe_mode_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    esp_err_t err = ctx->services.clear_safe_mode ?
                    ctx->services.clear_safe_mode() : ESP_ERR_NOT_SUPPORTED;
    cJSON *root = NULL;

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to clear safe mode");
    }

    root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "message", "safe mode cleared; restart scheduled");
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_status_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/restart", .method = HTTP_POST, .handler = restart_handler },
        { .uri = "/api/safe-mode/clear", .method = HTTP_POST, .handler = clear_safe_mode_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
