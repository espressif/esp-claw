/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_unitv_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

static const char *TAG = "cap_unitv_capture";

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} dynbuf_t;

static esp_err_t dynbuf_append(dynbuf_t *db, const char *data, size_t n)
{
    if (db->len + n + 1 > db->cap) {
        size_t new_cap = db->cap ? db->cap * 2 : 1024;
        while (new_cap < db->len + n + 1) {
            new_cap *= 2;
        }
        char *p = (char *)realloc(db->buf, new_cap);
        if (!p) {
            return ESP_ERR_NO_MEM;
        }
        db->buf = p;
        db->cap = new_cap;
    }

    memcpy(db->buf + db->len, data, n);
    db->len += n;
    db->buf[db->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    dynbuf_t *db = (dynbuf_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && db) {
        return dynbuf_append(db, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

/*
 * Build request body in one contiguous buffer without a separate base64 allocation.
 * Peak memory: jpeg_size (caller holds it) + body buffer (~jpeg_size * 4/3 + overhead).
 */
static esp_err_t build_request_body(const char *model,
                                    const char *question,
                                    const uint8_t *jpeg,
                                    size_t jpeg_size,
                                    uint32_t max_tokens,
                                    bool is_anthropic,
                                    char **out_body,
                                    size_t *out_len)
{
    char *q_json = NULL;
    char hdr[512];
    int hdr_len;
    size_t b64_needed = 0;
    size_t b64_written = 0;
    const char *suffix;
    size_t suffix_len;
    size_t total;
    char *body = NULL;
    int rc;

    /* Escape question as a JSON string value (includes surrounding quotes). */
    cJSON *q_node = cJSON_CreateString(question ? question : "");
    q_json = q_node ? cJSON_PrintUnformatted(q_node) : NULL;
    cJSON_Delete(q_node);
    if (!q_json) {
        return ESP_ERR_NO_MEM;
    }

    /* Calculate base64 output size. */
    mbedtls_base64_encode(NULL, 0, &b64_needed, jpeg, jpeg_size);

    if (is_anthropic) {
        hdr_len = snprintf(hdr, sizeof(hdr),
                           "{\"model\":\"%s\",\"max_tokens\":%u,"
                           "\"messages\":[{\"role\":\"user\",\"content\":["
                           "{\"type\":\"image\",\"source\":{\"type\":\"base64\","
                           "\"media_type\":\"image/jpeg\",\"data\":\"",
                           model, (unsigned)max_tokens);
        suffix = "\"}},{\"type\":\"text\",\"text\":%s}]}]}";
    } else {
        hdr_len = snprintf(hdr, sizeof(hdr),
                           "{\"model\":\"%s\",\"max_tokens\":%u,"
                           "\"messages\":[{\"role\":\"user\",\"content\":["
                           "{\"type\":\"text\",\"text\":%s},"
                           "{\"type\":\"image_url\",\"image_url\":{\"url\":"
                           "\"data:image/jpeg;base64,",
                           model, (unsigned)max_tokens, q_json);
        suffix = "\"}}]}]}";
    }

    if (hdr_len < 0 || hdr_len >= (int)sizeof(hdr)) {
        free(q_json);
        return ESP_ERR_NO_MEM;
    }

    /* For Anthropic, suffix includes the question JSON string. */
    char suffix_buf[320];
    if (is_anthropic) {
        snprintf(suffix_buf, sizeof(suffix_buf), suffix, q_json);
        suffix = suffix_buf;
    }
    free(q_json);

    suffix_len = strlen(suffix);
    total = (size_t)hdr_len + b64_needed + suffix_len;

    body = (char *)malloc(total + 1);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(body, hdr, (size_t)hdr_len);
    rc = mbedtls_base64_encode((unsigned char *)body + hdr_len,
                               b64_needed, &b64_written, jpeg, jpeg_size);
    if (rc != 0) {
        free(body);
        return ESP_FAIL;
    }
    memcpy(body + hdr_len + b64_written, suffix, suffix_len);
    body[hdr_len + b64_written + suffix_len] = '\0';

    *out_body = body;
    *out_len = hdr_len + b64_written + suffix_len;
    return ESP_OK;
}

static esp_err_t extract_openai_text(const char *resp_json, char *out, size_t out_size)
{
    cJSON *root = cJSON_Parse(resp_json);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *first = cJSON_GetArrayItem(choices, 0);
        cJSON *message = first ? cJSON_GetObjectItem(first, "message") : NULL;
        cJSON *content = message ? cJSON_GetObjectItem(message, "content") : NULL;
        if (cJSON_IsString(content) && content->valuestring) {
            strlcpy(out, content->valuestring, out_size);
            err = ESP_OK;
        }
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t extract_anthropic_text(const char *resp_json, char *out, size_t out_size)
{
    cJSON *root = cJSON_Parse(resp_json);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (cJSON_IsArray(content)) {
        for (int i = 0; i < cJSON_GetArraySize(content); i++) {
            cJSON *block = cJSON_GetArrayItem(content, i);
            cJSON *type = block ? cJSON_GetObjectItem(block, "type") : NULL;
            cJSON *text = block ? cJSON_GetObjectItem(block, "text") : NULL;
            if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 &&
                    cJSON_IsString(text) && text->valuestring) {
                strlcpy(out, text->valuestring, out_size);
                err = ESP_OK;
                break;
            }
        }
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t cap_unitv_vision_call(const char *question, const uint8_t *jpeg, size_t jpeg_size,
                                char *resp, size_t resp_size)
{
    if (!resp || resp_size == 0 || !jpeg || jpeg_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_cap_unitv.vision_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    bool is_anthropic = strcmp(g_cap_unitv.vision_backend_type, "anthropic") == 0;

    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = build_request_body(g_cap_unitv.vision_model,
                                       question,
                                       jpeg, jpeg_size,
                                       g_cap_unitv.vision_cfg.max_response_tokens,
                                       is_anthropic,
                                       &body, &body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "build_request_body failed: %s jpeg=%u", esp_err_to_name(err), (unsigned)jpeg_size);
        return err;
    }

    char url[256] = {0};
    const char *base = g_cap_unitv.vision_base_url[0] ? g_cap_unitv.vision_base_url :
                       (is_anthropic ? "https://api.anthropic.com/v1" : "https://api.openai.com/v1");
    snprintf(url, sizeof(url), "%s%s", base, is_anthropic ? "/messages" : "/chat/completions");

    dynbuf_t resp_buf = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)g_cap_unitv.vision_cfg.timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &resp_buf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (is_anthropic) {
        esp_http_client_set_header(client, "x-api-key", g_cap_unitv.vision_api_key);
        esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
    } else {
        char auth[256] = {0};
        snprintf(auth, sizeof(auth), "Bearer %s", g_cap_unitv.vision_api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, body, (int)body_len);

    ESP_LOGI(TAG, "vision POST %s jpeg=%u body=%u", url, (unsigned)jpeg_size, (unsigned)body_len);

    esp_err_t http_err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (http_err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "vision HTTP failed err=%s status=%d", esp_err_to_name(http_err), status);
        free(resp_buf.buf);
        return http_err != ESP_OK ? http_err : ESP_FAIL;
    }

    err = is_anthropic ? extract_anthropic_text(resp_buf.buf, resp, resp_size) :
          extract_openai_text(resp_buf.buf, resp, resp_size);
    free(resp_buf.buf);
    return err;
}
