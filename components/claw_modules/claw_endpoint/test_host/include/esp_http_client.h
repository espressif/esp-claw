#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct mock_http_client *esp_http_client_handle_t;
typedef struct {
    const char *url;
    bool is_async;
    bool keep_alive_enable;
    int timeout_ms;
    int method;
    int buffer_size;
    int buffer_size_tx;
    esp_err_t (*crt_bundle_attach)(void *);
} esp_http_client_config_t;
#define HTTP_METHOD_POST 1
enum { HttpStatus_MovedPermanently = 301, HttpStatus_Found = 302, HttpStatus_SeeOther = 303,
       HttpStatus_TemporaryRedirect = 307, HttpStatus_PermanentRedirect = 308 };
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define ESP_ERR_HTTP_MAX_REDIRECT 0x7001
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t client);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, int method);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int size);
int esp_http_client_write(esp_http_client_handle_t client, const char *data, int size);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *data, int size);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
