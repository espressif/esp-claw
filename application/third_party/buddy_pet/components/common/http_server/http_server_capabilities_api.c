/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include "app_capabilities.h"

static esp_err_t capabilities_get_handler(httpd_req_t *req)
{
    const app_capability_group_info_t *groups = NULL;
    size_t group_count = 0;
    cJSON *root = NULL;
    cJSON *items = NULL;
    esp_err_t err;

    err = app_capabilities_get_compiled_groups(&groups, &group_count);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load capabilities");
    }

    root = cJSON_CreateObject();
    items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToObject(root, "items", items);
    for (size_t i = 0; i < group_count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }

        http_server_json_add_string(item, "group_id", groups[i].group_id);
        http_server_json_add_string(item, "display_name", groups[i].display_name);
        cJSON_AddBoolToObject(item, "default_llm_visible", groups[i].llm_visible_by_default);
        cJSON_AddItemToArray(items, item);
    }

    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_capabilities_routes(httpd_handle_t server)
{
    const httpd_uri_t handler = {
        .uri = "/api/capabilities",
        .method = HTTP_GET,
        .handler = capabilities_get_handler,
    };

    return httpd_register_uri_handler(server, &handler);
}
