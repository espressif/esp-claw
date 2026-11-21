/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/claw_llm_http_transport.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "claw_http.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "llm_http";

#define CLAW_LLM_HTTP_RB_INITIAL_CAP 4096
#define CLAW_LLM_HTTP_STATUS_UNAUTHORIZED 401

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} response_buffer_t;

static bool request_cancelled(void *ctx)
{
    return ctx && *(volatile bool *)ctx;
}

static char *dup_printf(const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;
    char *buf;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    buf = calloc(1, (size_t)needed + 1);
    if (!buf) {
        va_end(args);
        return NULL;
    }

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
}

static char *sanitize_utf8_body_copy(const char *body)
{
    size_t src = 0;
    size_t dst = 0;
    size_t len = 0;
    char *sanitized = NULL;

    if (!body) {
        return NULL;
    }

    len = strlen(body);
    sanitized = calloc(1, len + 1);
    if (!sanitized) {
        return NULL;
    }

    while (body[src]) {
        unsigned char c = (unsigned char)body[src];

        if (c < 0x80) {
            sanitized[dst++] = body[src++];
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (body[src + 1] && ((unsigned char)body[src + 1] & 0xC0) == 0x80) {
                sanitized[dst++] = body[src++];
                sanitized[dst++] = body[src++];
            } else {
                sanitized[dst++] = ' ';
                src++;
            }
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (body[src + 1] && body[src + 2] &&
                    ((unsigned char)body[src + 1] & 0xC0) == 0x80 &&
                    ((unsigned char)body[src + 2] & 0xC0) == 0x80) {
                sanitized[dst++] = body[src++];
                sanitized[dst++] = body[src++];
                sanitized[dst++] = body[src++];
            } else {
                sanitized[dst++] = ' ';
                src++;
            }
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (body[src + 1] && body[src + 2] && body[src + 3] &&
                    ((unsigned char)body[src + 1] & 0xC0) == 0x80 &&
                    ((unsigned char)body[src + 2] & 0xC0) == 0x80 &&
                    ((unsigned char)body[src + 3] & 0xC0) == 0x80) {
                sanitized[dst++] = body[src++];
                sanitized[dst++] = body[src++];
                sanitized[dst++] = body[src++];
                sanitized[dst++] = body[src++];
            } else {
                sanitized[dst++] = ' ';
                src++;
            }
        } else {
            sanitized[dst++] = ' ';
            src++;
        }
    }

    sanitized[dst] = '\0';
    return sanitized;
}

static esp_err_t response_buffer_init(response_buffer_t *buffer)
{
    if (!buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    buffer->data = calloc(1, CLAW_LLM_HTTP_RB_INITIAL_CAP);
    if (!buffer->data) {
        return ESP_ERR_NO_MEM;
    }

    buffer->cap = CLAW_LLM_HTTP_RB_INITIAL_CAP;
    buffer->len = 0;
    return ESP_OK;
}

static esp_err_t response_buffer_append(response_buffer_t *buffer, const char *data, size_t len)
{
    char *grown;
    size_t cap;

    if (!buffer || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    cap = buffer->cap;
    while (buffer->len + len + 1 > cap) {
        cap *= 2;
    }

    if (cap != buffer->cap) {
        grown = realloc(buffer->data, cap);
        if (!grown) {
            return ESP_ERR_NO_MEM;
        }
        buffer->data = grown;
        buffer->cap = cap;
    }

    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return ESP_OK;
}

static void response_buffer_free(response_buffer_t *buffer)
{
    if (!buffer) {
        return;
    }

    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static esp_err_t receive_body(const char *data, size_t size, void *ctx)
{
    return response_buffer_append(ctx, data, size);
}

static char *join_url(const char *base_url, const char *path)
{
    bool base_has_slash = base_url[0] && base_url[strlen(base_url) - 1] == '/';
    bool path_has_slash = path[0] == '/';

    if (!path[0]) {
        return strdup(base_url);
    }
    if (base_has_slash && path_has_slash) {
        return dup_printf("%s%s", base_url, path + 1);
    }
    if (!base_has_slash && !path_has_slash) {
        return dup_printf("%s/%s", base_url, path);
    }
    return dup_printf("%s%s", base_url, path);
}

static char *parse_error_message_body(const char *body, int status)
{
    cJSON *root;
    cJSON *error;
    cJSON *message;
    char *fallback;

    if (!body || !body[0]) {
        return dup_printf("HTTP %d", status);
    }

    root = cJSON_Parse(body);
    if (!root) {
        return dup_printf("HTTP %d: %.160s", status, body);
    }

    error = cJSON_GetObjectItem(root, "error");
    if (error && cJSON_IsObject(error)) {
        message = cJSON_GetObjectItem(error, "message");
        if (message && cJSON_IsString(message) && message->valuestring[0]) {
            fallback = dup_printf("HTTP %d: %s", status, message->valuestring);
            cJSON_Delete(root);
            return fallback;
        }
    }

    message = cJSON_GetObjectItem(root, "message");
    if (message && cJSON_IsString(message) && message->valuestring[0]) {
        fallback = dup_printf("HTTP %d: %s", status, message->valuestring);
        cJSON_Delete(root);
        return fallback;
    }

    cJSON_Delete(root);
    return dup_printf("HTTP %d: %.160s", status, body);
}

esp_err_t claw_llm_http_post_json(const claw_llm_http_json_request_t *request,
                                  claw_llm_http_response_t *out_response,
                                  char **out_error_message)
{
    response_buffer_t buffer = {0};
    claw_endpoint_request_t local_request;
    const claw_endpoint_request_t *budget;
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    char *sanitized_body = NULL;
    int status_code = 0;
    esp_err_t err;

    if (out_response) {
        memset(out_response, 0, sizeof(*out_response));
    }
    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!request || !request->url || !request->body || !out_response || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    sanitized_body = sanitize_utf8_body_copy(request->body);
    if (!sanitized_body) {
        *out_error_message = dup_printf("Out of memory sanitizing HTTP request body");
        ESP_LOGE(TAG, "OOM sanitizing HTTP request body");
        return ESP_ERR_NO_MEM;
    }

    err = response_buffer_init(&buffer);
    if (err != ESP_OK) {
        *out_error_message = dup_printf("Out of memory allocating HTTP buffer");
        ESP_LOGE(TAG, "OOM allocating HTTP response buffer");
        goto cleanup;
    }

    claw_endpoint_request_init(&local_request, request->timeout_ms,
                                request_cancelled, (void *)request->abort_flag);
    budget = request->endpoint_request ? request->endpoint_request : &local_request;
    err = claw_endpoint_request_check(budget);
    if (err != ESP_OK) {
        *out_error_message = dup_printf("HTTP request stopped: %s", claw_endpoint_err_to_name(err));
        goto cleanup;
    }
    config.url = request->url;
    config.is_async = true;
    config.timeout_ms = claw_endpoint_request_remaining_ms(budget);
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    /* This async streaming client owns its lifecycle. The perform-based reuse
     * pool may contain synchronous clients, so this path does not opt in. */

    client = esp_http_client_init(&config);
    if (!client) {
        *out_error_message = dup_printf("Failed to create HTTP client");
        ESP_LOGE(TAG, "Failed to create HTTP client for %s", request->url);
        err = ESP_FAIL;
        goto cleanup;
    }

    err = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err != ESP_OK) {
        *out_error_message = dup_printf("Failed to configure HTTP request");
        goto cleanup;
    }
    if (request->headers && request->header_count > 0) {
        size_t i;

        for (i = 0; i < request->header_count; i++) {
            const claw_llm_http_header_t *header = &request->headers[i];

            if (!header->name || !header->name[0] || !header->value) {
                continue;
            }
            err = esp_http_client_set_header(client, header->name, header->value);
            if (err != ESP_OK) {
                *out_error_message = dup_printf("Failed to set HTTP request header: %s", header->name);
                goto cleanup;
            }
        }
    }

    ESP_LOGD(TAG, "POST %s", request->url);
    err = claw_http_send(client, sanitized_body, budget, receive_body, &buffer);
    if (err != ESP_OK) {
        *out_error_message = dup_printf("HTTP request failed: %s", claw_endpoint_err_to_name(err));
        goto cleanup;
    }

    status_code = esp_http_client_get_status_code(client);
    out_response->status_code = status_code;
    ESP_LOGD(TAG, "HTTP status=%d", status_code);
    if (status_code != 200) {
        err = ESP_FAIL;
        *out_error_message = parse_error_message_body(buffer.data, status_code);
        ESP_LOGE(TAG, "LLM error: %s", *out_error_message ? *out_error_message : "(null)");
        goto cleanup;
    }

    out_response->status_code = status_code;
    out_response->body = buffer.data;
    buffer.data = NULL;
    err = ESP_OK;

cleanup:
    free(sanitized_body);
    if (client) {
        esp_http_client_cleanup(client);
    }
    response_buffer_free(&buffer);
    return err;
}

static char *endpoint_error_message(esp_err_t err)
{
    if (err == CLAW_ENDPOINT_ERR_CREDENTIAL_EXCHANGE) {
        return dup_printf("LLM key exchange failed");
    }
    return dup_printf("LLM endpoint is unavailable: %s", claw_endpoint_err_to_name(err));
}

static esp_err_t credentials_rejected(char **error_message)
{
    char *detail = *error_message;

    *error_message = dup_printf("LLM verification failed: %s", detail ? detail : "HTTP 401");
    free(detail);
    return CLAW_ENDPOINT_ERR_CREDENTIAL_REJECTED;
}

static esp_err_t post_endpoint_once(claw_endpoint_handle_t endpoint,
                                    const char *path,
                                    const claw_llm_http_json_request_t *request,
                                    const claw_endpoint_request_t *budget,
                                    claw_llm_http_response_t *out_response,
                                    char **out_error_message,
                                    bool *out_retry)
{
    claw_endpoint_resolved_t resolved = {0};
    claw_llm_http_json_request_t resolved_request = *request;
    claw_llm_http_header_t *headers = NULL;
    size_t header_count;
    char *url = NULL;
    esp_err_t err;

    *out_retry = false;
    err = claw_endpoint_acquire(endpoint, budget, &resolved);
    if (err != ESP_OK) {
        *out_error_message = endpoint_error_message(err);
        ESP_LOGE(TAG, "%s", *out_error_message ? *out_error_message : "LLM endpoint is unavailable");
        return err;
    }

    header_count = resolved.header_count + request->header_count;
    url = join_url(resolved.url, path);
    headers = header_count ? calloc(header_count, sizeof(*headers)) : NULL;
    if (!url || (header_count && !headers)) {
        *out_error_message = dup_printf("Out of memory building HTTP request");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    for (size_t i = 0; i < resolved.header_count; i++) {
        headers[i].name = resolved.headers[i].name;
        headers[i].value = resolved.headers[i].value;
    }
    for (size_t i = 0; i < request->header_count; i++) {
        for (size_t j = 0; j < resolved.header_count; j++) {
            if (request->headers[i].name &&
                    strcasecmp(request->headers[i].name, resolved.headers[j].name) == 0) {
                *out_error_message = dup_printf("Protocol header conflicts with endpoint header: %s",
                                                resolved.headers[j].name);
                err = ESP_ERR_INVALID_ARG;
                goto cleanup;
            }
        }
        headers[resolved.header_count + i] = request->headers[i];
    }

    resolved_request.url = url;
    resolved_request.endpoint_request = budget;
    resolved_request.headers = headers;
    resolved_request.header_count = header_count;
    err = claw_llm_http_post_json(&resolved_request, out_response, out_error_message);
    if (err == ESP_OK) {
        (void)claw_endpoint_report(endpoint, &resolved, CLAW_ENDPOINT_OUTCOME_ACCEPTED);
    } else if (out_response->status_code == CLAW_LLM_HTTP_STATUS_UNAUTHORIZED) {
        *out_retry = claw_endpoint_report(endpoint, &resolved, CLAW_ENDPOINT_OUTCOME_REJECTED);
        if (!*out_retry) {
            err = credentials_rejected(out_error_message);
        }
    }

cleanup:
    free(headers);
    free(url);
    claw_endpoint_resolved_free(&resolved);
    return err;
}

esp_err_t claw_llm_http_post_endpoint(claw_endpoint_handle_t endpoint,
                                      const char *path,
                                      const claw_llm_http_json_request_t *request,
                                      claw_llm_http_response_t *out_response,
                                      char **out_error_message)
{
    bool retry = false;
    esp_err_t err;
    claw_endpoint_request_t budget;

    if (out_response) {
        memset(out_response, 0, sizeof(*out_response));
    }
    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!endpoint || !path || !request || request->url || !request->body ||
            (request->header_count && !request->headers) || !out_response || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_endpoint_request_init(&budget, request->timeout_ms,
                                request_cancelled, (void *)request->abort_flag);
    err = post_endpoint_once(endpoint, path, request, &budget, out_response, out_error_message, &retry);
    if (!retry) {
        return err;
    }

    ESP_LOGW(TAG, "Endpoint credentials rejected, retrying once with new credentials");
    free(*out_error_message);
    *out_error_message = NULL;
    err = post_endpoint_once(endpoint, path, request, &budget, out_response, out_error_message, &retry);
    if (retry) {
        err = credentials_rejected(out_error_message);
    }
    return err;
}

void claw_llm_http_response_free(claw_llm_http_response_t *response)
{
    if (!response) {
        return;
    }

    free(response->body);
    memset(response, 0, sizeof(*response));
}
