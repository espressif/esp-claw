/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_voice_tts.h"
#include "cap_voice_audio.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cap_voice_tts";

/* Dynamic response buffer grown on each HTTP event */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} tts_buf_t;

static esp_err_t on_data(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    tts_buf_t *b = (tts_buf_t *)evt->user_data;
    if (b->len + evt->data_len > b->cap) {
        size_t new_cap = b->cap + evt->data_len + 65536;
        uint8_t *tmp = heap_caps_realloc(b->data, new_cap,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp) return ESP_ERR_NO_MEM;
        b->data = tmp;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    return ESP_OK;
}

esp_err_t cap_voice_tts_speak(const cap_voice_tts_config_t *cfg,
                               const char *text)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", "tts-1");
    cJSON_AddStringToObject(body, "input", text);
    cJSON_AddStringToObject(body, "voice",
        cfg->voice ? cfg->voice : "alloy");
    cJSON_AddStringToObject(body, "response_format", "pcm");
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return ESP_ERR_NO_MEM;

    char url[256];
    snprintf(url, sizeof(url), "%s/v1/audio/speech",
             cfg->base_url ? cfg->base_url : "https://api.openai.com");

    char auth[512];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key ? cfg->api_key : "");

    tts_buf_t buf = {
        .data = heap_caps_malloc(65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .len  = 0,
        .cap  = 65536,
    };
    if (!buf.data) { free(body_str); return ESP_ERR_NO_MEM; }

    esp_http_client_config_t http_cfg = {
        .url              = url,
        .event_handler    = on_data,
        .user_data        = &buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms       = 30000,
        .buffer_size_tx   = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body_str, strlen(body_str));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "TTS HTTP status=%d pcm_bytes=%u", status, (unsigned)buf.len);
    esp_http_client_cleanup(client);
    free(body_str);

    if (err == ESP_OK && status == 200 && buf.len > 0) {
        err = cap_voice_audio_play((const int16_t *)buf.data,
                                    buf.len / sizeof(int16_t));
    } else {
        if (err == ESP_OK) err = ESP_FAIL;
    }
    free(buf.data);
    return err;
}
