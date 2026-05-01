/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cap_voice_priv.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "cap_voice_stt";

#define STT_TIMEOUT_MS      30000
#define STT_MODEL           "FunAudioLLM/SenseVoiceSmall"
#define STT_BASE_URL        "https://api.siliconflow.cn/v1"
#define BOUNDARY            "----VoiceBoundary9876543210"

/* HTTP response accumulator */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_buf_t;

static esp_err_t stt_http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_buf_t *resp = (http_resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t needed = resp->len + evt->data_len + 1;
        if (needed > resp->cap) {
            size_t new_cap = needed + 1024;
            char *new_buf = heap_caps_realloc(resp->buf, new_cap, MALLOC_CAP_SPIRAM);
            if (!new_buf) {
                return ESP_ERR_NO_MEM;
            }
            resp->buf = new_buf;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

esp_err_t cap_voice_stt_transcribe(const uint8_t *wav_data, size_t wav_len,
                                   const char *api_key, const char *base_url,
                                   char **out_text)
{
    if (!wav_data || wav_len == 0 || !api_key || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    /* Build URL: use SiliconFlow STT endpoint (separate from LLM base_url) */
    char url[256];
    snprintf(url, sizeof(url), "%s/audio/transcriptions", STT_BASE_URL);

    /* Build multipart body */
    const char *part_model_fmt =
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s\r\n";
    const char *part_file_hdr_fmt =
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    const char *part_end_fmt = "\r\n--%s--\r\n";

    /* Calculate total size */
    size_t model_part_len = strlen(BOUNDARY) + strlen(STT_MODEL) + 80;
    size_t file_hdr_len = strlen(BOUNDARY) + 120;
    size_t end_len = strlen(BOUNDARY) + 10;
    size_t total_len = model_part_len + file_hdr_len + wav_len + end_len;

    uint8_t *body = heap_caps_malloc(total_len + 256, MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "Failed to alloc multipart body");
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    offset += snprintf((char *)body + offset, total_len + 256 - offset, part_model_fmt, BOUNDARY, STT_MODEL);
    offset += snprintf((char *)body + offset, total_len + 256 - offset, part_file_hdr_fmt, BOUNDARY);
    memcpy(body + offset, wav_data, wav_len);
    offset += wav_len;
    offset += snprintf((char *)body + offset, total_len + 256 - offset, part_end_fmt, BOUNDARY);

    /* Prepare HTTP response buffer */
    http_resp_buf_t resp = {
        .buf = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM),
        .len = 0,
        .cap = 2048,
    };
    if (!resp.buf) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    /* Build auth header */
    char auth_header[384];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    /* Content-Type header */
    char content_type[80];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", BOUNDARY);

    /* HTTP request */
    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = STT_TIMEOUT_MS,
        .event_handler = stt_http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        free(body);
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, (const char *)body, offset);

    ESP_LOGI(TAG, "Sending %zu bytes to STT API: %s", offset, url);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return err;
    }

    ESP_LOGI(TAG, "STT response status=%d len=%zu", status, resp.len);

    if (status != 200) {
        ESP_LOGE(TAG, "STT API error %d: %s", status, resp.buf ? resp.buf : "(empty)");
        free(resp.buf);
        return ESP_FAIL;
    }

    /* Parse JSON response: {"text": "..."} */
    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse STT response JSON");
        return ESP_FAIL;
    }

    cJSON *text_item = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text_item) || !text_item->valuestring[0]) {
        ESP_LOGW(TAG, "STT returned empty text");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    *out_text = strdup(text_item->valuestring);
    cJSON_Delete(root);

    if (!*out_text) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
