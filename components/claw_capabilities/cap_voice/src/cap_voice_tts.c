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

static const char *TAG = "cap_voice_tts";

#define TTS_TIMEOUT_MS      30000
#define TTS_MODEL           "speech-2.8-hd"
#define TTS_VOICE_ID        "female-tianmei"
#define TTS_SAMPLE_RATE     16000

/* HTTP response accumulator */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_buf_t;

static esp_err_t tts_http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_buf_t *resp = (http_resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t needed = resp->len + evt->data_len + 1;
        if (needed > resp->cap) {
            size_t new_cap = needed * 2;
            if (new_cap < needed + 65536) new_cap = needed + 65536;
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

/* Decode hex string to binary. Returns allocated buffer (caller must free). */
static uint8_t *hex_decode(const char *hex, size_t hex_len, size_t *out_len)
{
    if (hex_len % 2 != 0) {
        return NULL;
    }
    size_t bin_len = hex_len / 2;
    uint8_t *bin = heap_caps_malloc(bin_len, MALLOC_CAP_SPIRAM);
    if (!bin) {
        return NULL;
    }

    for (size_t i = 0; i < bin_len; i++) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];
        uint8_t val = 0;

        if (hi >= '0' && hi <= '9') val = (hi - '0') << 4;
        else if (hi >= 'a' && hi <= 'f') val = (hi - 'a' + 10) << 4;
        else if (hi >= 'A' && hi <= 'F') val = (hi - 'A' + 10) << 4;
        else { free(bin); return NULL; }

        if (lo >= '0' && lo <= '9') val |= (lo - '0');
        else if (lo >= 'a' && lo <= 'f') val |= (lo - 'a' + 10);
        else if (lo >= 'A' && lo <= 'F') val |= (lo - 'A' + 10);
        else { free(bin); return NULL; }

        bin[i] = val;
    }
    *out_len = bin_len;
    return bin;
}

/* Base64 decode (simple implementation for TTS response) */
static const uint8_t b64_table[256] = {
    [0 ... 255] = 255,
    ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,  ['F'] = 5,
    ['G'] = 6,  ['H'] = 7,  ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11,
    ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15, ['Q'] = 16, ['R'] = 17,
    ['S'] = 18, ['T'] = 19, ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
    ['Y'] = 24, ['Z'] = 25,
    ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31,
    ['g'] = 32, ['h'] = 33, ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37,
    ['m'] = 38, ['n'] = 39, ['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43,
    ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49,
    ['y'] = 50, ['z'] = 51,
    ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55, ['4'] = 56, ['5'] = 57,
    ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61, ['+'] = 62, ['/'] = 63,
};

static uint8_t *base64_decode(const char *src, size_t src_len, size_t *out_len)
{
    /* Remove padding from length calculation */
    size_t pad = 0;
    if (src_len > 0 && src[src_len - 1] == '=') pad++;
    if (src_len > 1 && src[src_len - 2] == '=') pad++;

    size_t decoded_len = (src_len / 4) * 3 - pad;
    uint8_t *out = heap_caps_malloc(decoded_len + 4, MALLOC_CAP_SPIRAM);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < src_len; i += 4) {
        uint32_t sextet_a = (i < src_len) ? b64_table[(uint8_t)src[i]] : 0;
        uint32_t sextet_b = (i + 1 < src_len) ? b64_table[(uint8_t)src[i + 1]] : 0;
        uint32_t sextet_c = (i + 2 < src_len) ? b64_table[(uint8_t)src[i + 2]] : 0;
        uint32_t sextet_d = (i + 3 < src_len) ? b64_table[(uint8_t)src[i + 3]] : 0;

        if (sextet_a == 255 || sextet_b == 255) break;

        uint32_t triple = (sextet_a << 18) | (sextet_b << 12) | (sextet_c << 6) | sextet_d;
        if (j < decoded_len) out[j++] = (triple >> 16) & 0xFF;
        if (j < decoded_len) out[j++] = (triple >> 8) & 0xFF;
        if (j < decoded_len) out[j++] = triple & 0xFF;
    }
    *out_len = j;
    return out;
}

esp_err_t cap_voice_tts_synthesize(const char *text,
                                   const char *api_key, const char *base_url,
                                   uint8_t **out_pcm, size_t *out_pcm_len)
{
    if (!text || !api_key || !out_pcm || !out_pcm_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_pcm = NULL;
    *out_pcm_len = 0;

    /* Build URL */
    char url[256];
    const char *effective_base = (base_url && base_url[0]) ? base_url : "https://api.minimax.chat/v1";
    snprintf(url, sizeof(url), "%s/t2a_v2", effective_base);

    /* Build JSON request body */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", TTS_MODEL);
    cJSON_AddStringToObject(root, "text", text);

    cJSON *voice_setting = cJSON_CreateObject();
    cJSON_AddStringToObject(voice_setting, "voice_id", TTS_VOICE_ID);
    cJSON_AddNumberToObject(voice_setting, "speed", 1.0);
    cJSON_AddNumberToObject(voice_setting, "vol", 1.0);
    cJSON_AddNumberToObject(voice_setting, "pitch", 0);
    cJSON_AddItemToObject(root, "voice_setting", voice_setting);

    cJSON *audio_setting = cJSON_CreateObject();
    cJSON_AddNumberToObject(audio_setting, "sample_rate", TTS_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio_setting, "bitrate", 128000);
    cJSON_AddStringToObject(audio_setting, "format", "pcm");
    cJSON_AddNumberToObject(audio_setting, "channel", 1);
    cJSON_AddItemToObject(root, "audio_setting", audio_setting);

    cJSON_AddStringToObject(root, "output_format", "hex");

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    /* Prepare response buffer */
    http_resp_buf_t resp = {
        .buf = heap_caps_calloc(1, 65536, MALLOC_CAP_SPIRAM),
        .len = 0,
        .cap = 65536,
    };
    if (!resp.buf) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    /* Auth header */
    char auth_header[384];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    /* HTTP request */
    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TTS_TIMEOUT_MS,
        .event_handler = tts_http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        free(body);
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    ESP_LOGI(TAG, "Sending TTS request to %s (text_len=%d)", url, strlen(text));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return err;
    }

    ESP_LOGI(TAG, "TTS response status=%d len=%zu", status, resp.len);

    if (status != 200) {
        ESP_LOGE(TAG, "TTS API error %d: %.200s", status, resp.buf ? resp.buf : "(empty)");
        free(resp.buf);
        return ESP_FAIL;
    }

    /* Parse response JSON */
    cJSON *resp_root = cJSON_Parse(resp.buf);
    if (!resp_root) {
        ESP_LOGE(TAG, "Failed to parse TTS JSON response");
        free(resp.buf);
        return ESP_FAIL;
    }
    free(resp.buf);

    /* Check for error in response */
    cJSON *base_resp = cJSON_GetObjectItemCaseSensitive(resp_root, "base_resp");
    if (base_resp) {
        cJSON *status_code = cJSON_GetObjectItemCaseSensitive(base_resp, "status_code");
        if (status_code && cJSON_IsNumber(status_code) && status_code->valueint != 0) {
            cJSON *status_msg = cJSON_GetObjectItemCaseSensitive(base_resp, "status_msg");
            ESP_LOGE(TAG, "TTS API error: code=%d msg=%s",
                     status_code->valueint,
                     status_msg ? status_msg->valuestring : "unknown");
            cJSON_Delete(resp_root);
            return ESP_FAIL;
        }
    }

    /* Extract audio data */
    cJSON *data = cJSON_GetObjectItemCaseSensitive(resp_root, "data");
    if (!data) {
        data = cJSON_GetObjectItemCaseSensitive(resp_root, "audio");
    }

    /* Try hex format first */
    cJSON *audio_hex = cJSON_GetObjectItemCaseSensitive(data ? data : resp_root, "audio");
    if (!audio_hex || !cJSON_IsString(audio_hex)) {
        audio_hex = cJSON_GetObjectItemCaseSensitive(resp_root, "audio_file");
    }

    if (audio_hex && cJSON_IsString(audio_hex) && audio_hex->valuestring[0]) {
        const char *hex_str = audio_hex->valuestring;
        size_t hex_len = strlen(hex_str);

        /* Detect if it's hex or base64 */
        bool is_hex = true;
        for (size_t i = 0; i < 16 && i < hex_len; i++) {
            char c = hex_str[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                is_hex = false;
                break;
            }
        }

        if (is_hex && hex_len > 0) {
            *out_pcm = hex_decode(hex_str, hex_len, out_pcm_len);
        } else {
            *out_pcm = base64_decode(hex_str, hex_len, out_pcm_len);
        }

        if (!*out_pcm) {
            ESP_LOGE(TAG, "Failed to decode audio data");
            cJSON_Delete(resp_root);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "TTS decoded %zu bytes PCM audio", *out_pcm_len);
    } else {
        ESP_LOGE(TAG, "No audio data found in TTS response");
        cJSON_Delete(resp_root);
        return ESP_FAIL;
    }

    cJSON_Delete(resp_root);
    return ESP_OK;
}
