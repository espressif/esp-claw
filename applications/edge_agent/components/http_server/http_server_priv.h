/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "http_server.h"

#define HTTP_SERVER_CTRL_PORT         32769
#define HTTP_SERVER_SCRATCH_SIZE      4096
#define HTTP_SERVER_PATH_MAX          256
#define HTTP_SERVER_UPLOAD_MAX_SIZE   (512 * 1024)

typedef struct {
    httpd_handle_t server;
    char storage_base_path[HTTP_SERVER_PATH_MAX];
    http_server_services_t services;
} http_server_ctx_t;

http_server_ctx_t *http_server_ctx(void);

char *http_server_alloc_scratch_buffer(void);
bool http_server_path_is_safe(const char *path);
void http_server_url_decode_inplace(char *value);
esp_err_t http_server_query_get(httpd_req_t *req, const char *key, char *value, size_t value_size);
esp_err_t http_server_send_embedded_file(httpd_req_t *req,
                                         const uint8_t *start,
                                         const uint8_t *end,
                                         const char *content_type);
void http_server_json_add_string(cJSON *root, const char *key, const char *value);
esp_err_t http_server_send_json_response(httpd_req_t *req, cJSON *root);
esp_err_t http_server_parse_json_body(httpd_req_t *req, cJSON **out_root);
void http_server_json_read_string(cJSON *root, const char *key, char *buffer, size_t buffer_size);
esp_err_t http_server_resolve_storage_path(const char *relative_path, char *full_path, size_t full_path_size);
bool http_server_build_child_relative_path(const char *base_path,
                                           const char *entry_name,
                                           char *out_path,
                                           size_t out_path_size);

esp_err_t http_server_register_assets_routes(httpd_handle_t server);
esp_err_t http_server_register_capabilities_routes(httpd_handle_t server);
esp_err_t http_server_register_lua_modules_routes(httpd_handle_t server);
esp_err_t http_server_register_config_routes(httpd_handle_t server);
esp_err_t http_server_register_status_routes(httpd_handle_t server);
esp_err_t http_server_register_files_routes(httpd_handle_t server);
#if CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER
esp_err_t http_server_register_lua_app_routes(httpd_handle_t server);
#endif
esp_err_t http_server_register_wechat_routes(httpd_handle_t server);
esp_err_t http_server_register_webim_routes(httpd_handle_t server);
void http_server_webim_ws_fd_remove(int fd);
esp_err_t http_server_captive_404_handler(httpd_req_t *req, httpd_err_code_t error);
