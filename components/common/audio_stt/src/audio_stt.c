/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_stt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "audio_stt";

#define AUDIO_STT_MAX_KEY_LEN       320
#define AUDIO_STT_MAX_URL_LEN       320
#define AUDIO_STT_MAX_MODEL_LEN     64
#define AUDIO_STT_MAX_LANG_LEN      32
#define AUDIO_STT_MAX_BACKEND_LEN   32
#define AUDIO_STT_RESP_CAP          32768
#define AUDIO_STT_TIMEOUT_MS        45000
#define AUDIO_STT_MULTIPART_BOUNDARY "----audio_stt_boundary"

typedef struct {
    bool enabled;
    bool keep_audio_in_storage;
    char backend_type[AUDIO_STT_MAX_BACKEND_LEN];
    char api_key[AUDIO_STT_MAX_KEY_LEN];
    char base_url[AUDIO_STT_MAX_URL_LEN];
    char model[AUDIO_STT_MAX_MODEL_LEN];
    char language[AUDIO_STT_MAX_LANG_LEN];
    SemaphoreHandle_t lock;
} audio_stt_state_t;

static audio_stt_state_t s_stt = {0};

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} audio_stt_resp_t;

static esp_err_t audio_stt_lock(void)
{
    if (!s_stt.lock) {
        s_stt.lock = xSemaphoreCreateMutex();
        if (!s_stt.lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return xSemaphoreTake(s_stt.lock, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_FAIL;
}

static void audio_stt_unlock(void)
{
    if (s_stt.lock) {
        xSemaphoreGive(s_stt.lock);
    }
}

esp_err_t audio_stt_set_config(const audio_stt_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (audio_stt_lock() != ESP_OK) {
        return ESP_FAIL;
    }

    s_stt.enabled = config->enabled;
    s_stt.keep_audio_in_storage = config->keep_audio_in_storage;
    strlcpy(s_stt.backend_type,
            (config->backend_type && config->backend_type[0]) ? config->backend_type : "openai",
            sizeof(s_stt.backend_type));
    strlcpy(s_stt.api_key, config->api_key ? config->api_key : "", sizeof(s_stt.api_key));
    strlcpy(s_stt.base_url,
            (config->base_url && config->base_url[0]) ? config->base_url : "https://api.openai.com/v1",
            sizeof(s_stt.base_url));
    strlcpy(s_stt.model,
            (config->model && config->model[0]) ? config->model : "whisper-1",
            sizeof(s_stt.model));
    strlcpy(s_stt.language, config->language ? config->language : "", sizeof(s_stt.language));

    audio_stt_unlock();
    return ESP_OK;
}

bool audio_stt_is_enabled(void)
{
    bool enabled;
    if (audio_stt_lock() != ESP_OK) {
        return false;
    }
    enabled = s_stt.enabled && s_stt.api_key[0] != '\0';
    audio_stt_unlock();
    return enabled;
}

bool audio_stt_keep_audio_in_storage(void)
{
    bool keep;
    if (audio_stt_lock() != ESP_OK) {
        return false;
    }
    keep = s_stt.keep_audio_in_storage;
    audio_stt_unlock();
    return keep;
}

static esp_err_t audio_stt_http_event(esp_http_client_event_t *event)
{
    audio_stt_resp_t *resp = (audio_stt_resp_t *)event->user_data;

    if (!resp || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    size_t needed = resp->len + (size_t)event->data_len + 1;
    if (needed > resp->cap) {
        if (needed > AUDIO_STT_RESP_CAP) {
            needed = AUDIO_STT_RESP_CAP;
            if (resp->len >= needed - 1) {
                return ESP_OK;
            }
        }
        char *tmp = realloc(resp->data, needed);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        resp->data = tmp;
        resp->cap = needed;
    }

    size_t copy = (size_t)event->data_len;
    if (resp->len + copy >= resp->cap) {
        copy = resp->cap - resp->len - 1;
    }
    if (copy > 0) {
        memcpy(resp->data + resp->len, event->data, copy);
        resp->len += copy;
        resp->data[resp->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t audio_stt_write_all(esp_http_client_handle_t client,
                                     const char *data,
                                     size_t len)
{
    size_t total = 0;
    while (total < len) {
        int written = esp_http_client_write(client, data + total, (int)(len - total));
        if (written <= 0) {
            return ESP_FAIL;
        }
        total += (size_t)written;
    }
    return ESP_OK;
}

static esp_err_t audio_stt_stream_file(esp_http_client_handle_t client, FILE *file)
{
    char buf[1024];
    while (!feof(file)) {
        size_t n = fread(buf, 1, sizeof(buf), file);
        if (n > 0) {
            esp_err_t err = audio_stt_write_all(client, buf, n);
            if (err != ESP_OK) {
                return err;
            }
        }
        if (ferror(file)) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

typedef struct {
    const char *file_path;       /* non-NULL: read from FATFS file */
    const void *buffer;          /* non-NULL: read from RAM buffer */
    size_t      buffer_len;
    const char *filename;        /* basename used in multipart filename param */
} audio_source_t;

static esp_err_t audio_source_size(const audio_source_t *src, size_t *out_size)
{
    if (src->buffer) {
        *out_size = src->buffer_len;
        return src->buffer_len > 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    struct stat st;
    if (stat(src->file_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_size = (size_t)st.st_size;
    return ESP_OK;
}

static esp_err_t audio_stt_drain_body(esp_http_client_handle_t client)
{
    char buf[1024];
    int total = 0;
    while (1) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    (void)total;
    return ESP_OK;
}

static esp_err_t audio_source_stream(const audio_source_t *src, esp_http_client_handle_t client)
{
    if (src->buffer) {
        return audio_stt_write_all(client, src->buffer, src->buffer_len);
    }
    FILE *file = fopen(src->file_path, "rb");
    if (!file) {
        return ESP_FAIL;
    }
    esp_err_t err = audio_stt_stream_file(client, file);
    fclose(file);
    return err;
}

static const char *audio_stt_basename(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : path;
}

static const char *audio_stt_default_mime(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    if (!dot) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".oga") == 0 || strcasecmp(dot, ".ogg") == 0) {
        return "audio/ogg";
    }
    if (strcasecmp(dot, ".mp3") == 0) {
        return "audio/mpeg";
    }
    if (strcasecmp(dot, ".wav") == 0) {
        return "audio/wav";
    }
    if (strcasecmp(dot, ".m4a") == 0 || strcasecmp(dot, ".aac") == 0) {
        return "audio/mp4";
    }
    if (strcasecmp(dot, ".webm") == 0) {
        return "audio/webm";
    }
    return "application/octet-stream";
}

static esp_err_t audio_stt_extract_openai_text(const char *json_body, char *out, size_t out_size)
{
    cJSON *root = cJSON_Parse(json_body);
    if (!root) {
        return ESP_FAIL;
    }
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    esp_err_t err = ESP_FAIL;
    if (cJSON_IsString(text) && text->valuestring) {
        strlcpy(out, text->valuestring, out_size);
        err = ESP_OK;
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t audio_stt_extract_deepgram_text(const char *json_body, char *out, size_t out_size)
{
    cJSON *root = cJSON_Parse(json_body);
    if (!root) {
        return ESP_FAIL;
    }
    esp_err_t err = ESP_FAIL;
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(results, "channels");
    const cJSON *channel0 = cJSON_GetArrayItem(channels, 0);
    const cJSON *alts = cJSON_GetObjectItemCaseSensitive(channel0, "alternatives");
    const cJSON *alt0 = cJSON_GetArrayItem(alts, 0);
    const cJSON *transcript = cJSON_GetObjectItemCaseSensitive(alt0, "transcript");
    if (cJSON_IsString(transcript) && transcript->valuestring) {
        strlcpy(out, transcript->valuestring, out_size);
        err = ESP_OK;
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t audio_stt_transcribe_openai(const audio_source_t *src,
                                             const char *mime_hint,
                                             const audio_stt_state_t *snapshot,
                                             char *out_text,
                                             size_t out_text_size)
{
    size_t audio_size = 0;
    esp_err_t size_err = audio_source_size(src, &audio_size);
    if (size_err != ESP_OK) {
        return size_err;
    }

    char url[AUDIO_STT_MAX_URL_LEN + 64];
    snprintf(url, sizeof(url), "%s/audio/transcriptions", snapshot->base_url);

    const char *hint_path = src->file_path ? src->file_path : src->filename;
    const char *mime = (mime_hint && mime_hint[0]) ? mime_hint : audio_stt_default_mime(hint_path);
    const char *fname = src->filename ? src->filename
                                      : (src->file_path ? audio_stt_basename(src->file_path) : "audio");

    char part_model[160];
    int part_model_len = snprintf(part_model, sizeof(part_model),
        "--" AUDIO_STT_MULTIPART_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s\r\n",
        snapshot->model);

    char part_lang[160];
    int part_lang_len = 0;
    if (snapshot->language[0]) {
        part_lang_len = snprintf(part_lang, sizeof(part_lang),
            "--" AUDIO_STT_MULTIPART_BOUNDARY "\r\n"
            "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
            "%s\r\n",
            snapshot->language);
    }

    char part_fmt[160];
    int part_fmt_len = snprintf(part_fmt, sizeof(part_fmt),
        "--" AUDIO_STT_MULTIPART_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
        "json\r\n");

    char part_file[256];
    int part_file_len = snprintf(part_file, sizeof(part_file),
        "--" AUDIO_STT_MULTIPART_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: %s\r\n\r\n",
        fname, mime);

    const char closing[] = "\r\n--" AUDIO_STT_MULTIPART_BOUNDARY "--\r\n";
    int closing_len = (int)sizeof(closing) - 1;

    if (part_model_len <= 0 || part_fmt_len <= 0 || part_file_len <= 0 ||
            part_lang_len < 0) {
        return ESP_FAIL;
    }

    size_t content_length = (size_t)part_model_len + (size_t)part_lang_len +
                            (size_t)part_fmt_len + (size_t)part_file_len +
                            audio_size + (size_t)closing_len;

    audio_stt_resp_t resp = {0};
    resp.data = calloc(1, 512);
    resp.cap = resp.data ? 512 : 0;
    if (!resp.data) {
        return ESP_ERR_NO_MEM;
    }

    char auth_hdr[AUDIO_STT_MAX_KEY_LEN + 32];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", snapshot->api_key);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = audio_stt_http_event,
        .user_data = &resp,
        .timeout_ms = AUDIO_STT_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.data);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type",
        "multipart/form-data; boundary=" AUDIO_STT_MULTIPART_BOUNDARY);

    esp_err_t err = esp_http_client_open(client, (int)content_length);
    if (err == ESP_OK) err = audio_stt_write_all(client, part_model, (size_t)part_model_len);
    if (err == ESP_OK && part_lang_len > 0) err = audio_stt_write_all(client, part_lang, (size_t)part_lang_len);
    if (err == ESP_OK) err = audio_stt_write_all(client, part_fmt, (size_t)part_fmt_len);
    if (err == ESP_OK) err = audio_stt_write_all(client, part_file, (size_t)part_file_len);
    if (err == ESP_OK) err = audio_source_stream(src, client);
    if (err == ESP_OK) err = audio_stt_write_all(client, closing, (size_t)closing_len);
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (err == ESP_OK) err = audio_stt_drain_body(client);

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "openai stt http transport failed: %s", esp_err_to_name(err));
        free(resp.data);
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "openai stt status=%d body=%.300s", status, resp.data ? resp.data : "");
        free(resp.data);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "openai stt 200, body=%.200s", resp.data ? resp.data : "(empty)");
    err = audio_stt_extract_openai_text(resp.data ? resp.data : "", out_text, out_text_size);
    if (err == ESP_OK && out_text[0] == '\0') {
        ESP_LOGW(TAG, "openai stt returned empty transcript; full body=%.300s",
                 resp.data ? resp.data : "(empty)");
    }
    free(resp.data);
    return err;
}

static esp_err_t audio_stt_url_append_query(char *url, size_t cap, const char *key, const char *value)
{
    if (!url || !key || !value || !value[0]) {
        return ESP_OK;
    }
    size_t cur = strlen(url);
    char sep = strchr(url, '?') ? '&' : '?';
    int written = snprintf(url + cur, cap - cur, "%c%s=%s", sep, key, value);
    if (written < 0 || (size_t)written >= cap - cur) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t audio_stt_transcribe_deepgram(const audio_source_t *src,
                                               const char *mime_hint,
                                               const audio_stt_state_t *snapshot,
                                               char *out_text,
                                               size_t out_text_size)
{
    size_t audio_size = 0;
    esp_err_t size_err = audio_source_size(src, &audio_size);
    if (size_err != ESP_OK) {
        return size_err;
    }

    char url[AUDIO_STT_MAX_URL_LEN + 128];
    snprintf(url, sizeof(url), "%s/listen", snapshot->base_url);
    audio_stt_url_append_query(url, sizeof(url), "model", snapshot->model);
    if (snapshot->language[0]) {
        audio_stt_url_append_query(url, sizeof(url), "language", snapshot->language);
    } else {
        /* Without a language hint nova-2 silently uses the English-only model and
         * returns an empty transcript for non-English audio. detect_language=true
         * makes Deepgram pick the right model from the audio. */
        audio_stt_url_append_query(url, sizeof(url), "detect_language", "true");
    }
    audio_stt_url_append_query(url, sizeof(url), "smart_format", "true");

    const char *hint_path = src->file_path ? src->file_path : src->filename;
    const char *mime = (mime_hint && mime_hint[0]) ? mime_hint : audio_stt_default_mime(hint_path);

    audio_stt_resp_t resp = {0};
    resp.data = calloc(1, 512);
    resp.cap = resp.data ? 512 : 0;
    if (!resp.data) {
        return ESP_ERR_NO_MEM;
    }

    char auth_hdr[AUDIO_STT_MAX_KEY_LEN + 32];
    snprintf(auth_hdr, sizeof(auth_hdr), "Token %s", snapshot->api_key);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = audio_stt_http_event,
        .user_data = &resp,
        .timeout_ms = AUDIO_STT_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.data);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type", mime);

    esp_err_t err = esp_http_client_open(client, (int)audio_size);
    if (err == ESP_OK) err = audio_source_stream(src, client);
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (err == ESP_OK) err = audio_stt_drain_body(client);

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deepgram stt http transport failed: %s", esp_err_to_name(err));
        free(resp.data);
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "deepgram stt status=%d body=%.300s", status, resp.data ? resp.data : "");
        free(resp.data);
        return ESP_FAIL;
    }

    err = audio_stt_extract_deepgram_text(resp.data ? resp.data : "", out_text, out_text_size);
    if (err == ESP_OK && out_text[0]) {
        ESP_LOGI(TAG, "deepgram stt 200, len=%u",
                 (unsigned)(resp.data ? strlen(resp.data) : 0));
    } else {
        ESP_LOGW(TAG, "deepgram stt extract err=%s, transcript=%s, full body:",
                 esp_err_to_name(err), out_text[0] ? "<set>" : "<empty>");
        ESP_LOGW(TAG, "%s", resp.data ? resp.data : "(empty)");
    }
    free(resp.data);
    return err;
}

static esp_err_t audio_stt_dispatch(const audio_source_t *src,
                                    const char *mime_type,
                                    char *out_text,
                                    size_t out_text_size)
{
    if (!out_text || out_text_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';

    audio_stt_state_t snapshot;
    if (audio_stt_lock() != ESP_OK) {
        return ESP_FAIL;
    }
    memcpy(&snapshot, &s_stt, sizeof(snapshot));
    audio_stt_unlock();

    if (!snapshot.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!snapshot.api_key[0]) {
        ESP_LOGW(TAG, "stt enabled but api_key empty");
        return ESP_ERR_INVALID_STATE;
    }

    if (strcmp(snapshot.backend_type, "deepgram") == 0) {
        return audio_stt_transcribe_deepgram(src, mime_type, &snapshot, out_text, out_text_size);
    }
    return audio_stt_transcribe_openai(src, mime_type, &snapshot, out_text, out_text_size);
}

esp_err_t audio_stt_transcribe_file(const char *file_path,
                                    const char *mime_type,
                                    char *out_text,
                                    size_t out_text_size)
{
    if (!file_path || !file_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_source_t src = {
        .file_path = file_path,
        .filename = audio_stt_basename(file_path),
    };
    return audio_stt_dispatch(&src, mime_type, out_text, out_text_size);
}

esp_err_t audio_stt_transcribe_buffer(const void *audio_data,
                                      size_t audio_len,
                                      const char *filename,
                                      const char *mime_type,
                                      char *out_text,
                                      size_t out_text_size)
{
    if (!audio_data || audio_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_source_t src = {
        .buffer = audio_data,
        .buffer_len = audio_len,
        .filename = (filename && filename[0]) ? filename : "audio.bin",
    };
    return audio_stt_dispatch(&src, mime_type, out_text, out_text_size);
}
