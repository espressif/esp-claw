/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_feishu.h"

#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cap_im_attachment.h"
#include "claw_cap.h"
#include "claw_task.h"
#include "claw_event_publisher.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define CAP_IM_FEISHU_API_BASE "https://open.feishu.cn/open-apis"
#define CAP_IM_FEISHU_AUTH_URL CAP_IM_FEISHU_API_BASE "/auth/v3/tenant_access_token/internal"
#define CAP_IM_FEISHU_SEND_MSG_URL CAP_IM_FEISHU_API_BASE "/im/v1/messages"
#define CAP_IM_FEISHU_UPLOAD_IMAGE_URL CAP_IM_FEISHU_API_BASE "/im/v1/images"
#define CAP_IM_FEISHU_UPLOAD_FILE_URL CAP_IM_FEISHU_API_BASE "/im/v1/files"
#define CAP_IM_FEISHU_WS_CONFIG_URL "https://open.feishu.cn/callback/ws/endpoint"
#define CAP_IM_FEISHU_MULTIPART_BOUNDARY "----cap_im_feishu_boundary"

#define CAP_IM_FEISHU_APP_ID_LEN 64
#define CAP_IM_FEISHU_APP_SECRET_LEN 256
#define CAP_IM_FEISHU_TOKEN_LEN 512
#define CAP_IM_FEISHU_URL_LEN 512
#define CAP_IM_FEISHU_PATH_LEN 256
#define CAP_IM_FEISHU_NAME_LEN 96
#define CAP_IM_FEISHU_EVENT_ID_LEN 96
#define CAP_IM_FEISHU_MEDIA_KEY_LEN 256
#define CAP_IM_FEISHU_MAX_HEADERS 16
#define CAP_IM_FEISHU_MAX_RESPONSE 4096
#define CAP_IM_FEISHU_MAX_CHUNK_LEN 1800
#define CAP_IM_FEISHU_MAX_CARD_MARKDOWN_LEN 6000
#define CAP_IM_FEISHU_MAX_CARD_CONTENT_LEN 8192
#define CAP_IM_FEISHU_DEDUP_CACHE_SIZE 64
#define CAP_IM_FEISHU_RECONNECT_DELAY_MS 3000
#define CAP_IM_FEISHU_INITIAL_CONNECT_TIMEOUT_MS 15000
#define CAP_IM_FEISHU_ATTACHMENT_QUEUE_LEN 8

static const char *TAG = "cap_im_feishu";

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} cap_im_feishu_resp_t;

typedef struct {
    char key[32];
    char value[128];
} cap_im_feishu_ws_header_t;

typedef struct {
    uint64_t seq_id;
    uint64_t log_id;
    int32_t service;
    int32_t method;
    cap_im_feishu_ws_header_t headers[CAP_IM_FEISHU_MAX_HEADERS];
    size_t header_count;
    const uint8_t *payload;
    size_t payload_len;
} cap_im_feishu_ws_frame_t;

typedef struct {
    char *chat_id;
    char *sender_id;
    char *message_id;
    char *attachment_kind;
    char *resource_key;
    char *original_filename;
    char *content_json;
    char *content_type;
} cap_im_feishu_attachment_job_t;

typedef struct {
    char app_id[CAP_IM_FEISHU_APP_ID_LEN];
    char app_secret[CAP_IM_FEISHU_APP_SECRET_LEN];
    char tenant_token[CAP_IM_FEISHU_TOKEN_LEN];
    int64_t token_expire_time_ms;
    char ws_url[CAP_IM_FEISHU_URL_LEN];
    int ws_ping_interval_ms;
    int ws_reconnect_interval_ms;
    int ws_reconnect_nonce_ms;
    int ws_service_id;
    bool ws_connected;
    bool ws_ever_connected;
    int64_t ws_disconnect_since_ms;
    bool stop_requested;
    bool enable_inbound_attachments;
    size_t max_inbound_file_bytes;
    char attachment_root_dir[CAP_IM_FEISHU_PATH_LEN];
    esp_websocket_client_handle_t ws_client;
    TaskHandle_t ws_task;
    TaskHandle_t attachment_task;
    QueueHandle_t attachment_queue;
    uint64_t seen_message_keys[CAP_IM_FEISHU_DEDUP_CACHE_SIZE];
    size_t seen_message_idx;
} cap_im_feishu_state_t;

static cap_im_feishu_state_t s_feishu = {
    .ws_ping_interval_ms = 120000,
    .ws_reconnect_interval_ms = 30000,
    .ws_reconnect_nonce_ms = 30000,
};

static int64_t cap_im_feishu_now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static uint64_t cap_im_feishu_fnv1a64(const char *text)
{
    uint64_t hash = 1469598103934665603ULL;

    if (!text) {
        return hash;
    }

    while (*text) {
        hash ^= (uint8_t)(*text++);
        hash *= 1099511628211ULL;
    }

    return hash;
}

static bool cap_im_feishu_dedup_check_and_record(const char *message_id)
{
    uint64_t key = cap_im_feishu_fnv1a64(message_id);
    size_t i;

    for (i = 0; i < CAP_IM_FEISHU_DEDUP_CACHE_SIZE; i++) {
        if (s_feishu.seen_message_keys[i] == key) {
            return true;
        }
    }

    s_feishu.seen_message_keys[s_feishu.seen_message_idx] = key;
    s_feishu.seen_message_idx = (s_feishu.seen_message_idx + 1) % CAP_IM_FEISHU_DEDUP_CACHE_SIZE;
    return false;
}

static esp_err_t cap_im_feishu_resp_init(cap_im_feishu_resp_t *resp)
{
    if (!resp) {
        return ESP_ERR_INVALID_ARG;
    }

    resp->buf = calloc(1, CAP_IM_FEISHU_MAX_RESPONSE);
    if (!resp->buf) {
        return ESP_ERR_NO_MEM;
    }
    resp->cap = CAP_IM_FEISHU_MAX_RESPONSE;
    resp->len = 0;
    return ESP_OK;
}

static esp_err_t cap_im_feishu_resp_append(cap_im_feishu_resp_t *resp, const char *data, size_t len)
{
    char *grown = NULL;
    size_t cap = 0;

    if (!resp || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    cap = resp->cap;
    while (resp->len + len + 1 > cap) {
        cap *= 2;
    }

    if (cap != resp->cap) {
        grown = realloc(resp->buf, cap);
        if (!grown) {
            return ESP_ERR_NO_MEM;
        }
        resp->buf = grown;
        resp->cap = cap;
    }

    memcpy(resp->buf + resp->len, data, len);
    resp->len += len;
    resp->buf[resp->len] = '\0';
    return ESP_OK;
}

static void cap_im_feishu_resp_free(cap_im_feishu_resp_t *resp)
{
    if (!resp) {
        return;
    }

    free(resp->buf);
    memset(resp, 0, sizeof(*resp));
}

static esp_err_t cap_im_feishu_http_event_handler(esp_http_client_event_t *evt)
{
    cap_im_feishu_resp_t *resp = (cap_im_feishu_resp_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        return cap_im_feishu_resp_append(resp, (const char *)evt->data, evt->data_len);
    }

    return ESP_OK;
}

static esp_err_t cap_im_feishu_http_json(const char *url,
                                         const char *method,
                                         const char *authorization,
                                         const char *body,
                                         char **out_response,
                                         int *out_status)
{
    cap_im_feishu_resp_t resp = {0};
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t err;

    if (out_response) {
        *out_response = NULL;
    }
    if (out_status) {
        *out_status = 0;
    }
    if (!url || !method || !out_response || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_feishu_resp_init(&resp);
    if (err != ESP_OK) {
        return err;
    }

    config.url = url;
    config.event_handler = cap_im_feishu_http_event_handler;
    config.user_data = &resp;
    config.timeout_ms = 15000;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (!client) {
        cap_im_feishu_resp_free(&resp);
        return ESP_FAIL;
    }

    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    if (authorization && authorization[0]) {
        esp_http_client_set_header(client, "Authorization", authorization);
    }
    if (body) {
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        cap_im_feishu_resp_free(&resp);
        return err;
    }

    *out_response = resp.buf;
    resp.buf = NULL;
    cap_im_feishu_resp_free(&resp);
    return ESP_OK;
}

static esp_err_t cap_im_feishu_get_tenant_token(void)
{
    cJSON *body = NULL;
    char *json_str = NULL;
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *code_json = NULL;
    cJSON *token_json = NULL;
    cJSON *expire_json = NULL;
    int status = 0;
    int64_t now_ms = cap_im_feishu_now_ms();
    esp_err_t err = ESP_OK;

    if (s_feishu.tenant_token[0] && now_ms + 300000 < s_feishu.token_expire_time_ms) {
        return ESP_OK;
    }
    if (!s_feishu.app_id[0] || !s_feishu.app_secret[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "app_id", s_feishu.app_id);
    cJSON_AddStringToObject(body, "app_secret", s_feishu.app_secret);
    json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_feishu_http_json(CAP_IM_FEISHU_AUTH_URL, "POST", NULL, json_str, &response, &status);
    free(json_str);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Feishu token request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Feishu token request HTTP %d", status);
        free(response);
        return ESP_FAIL;
    }

    root = cJSON_Parse(response);
    free(response);
    if (!root) {
        ESP_LOGE(TAG, "Feishu token response parse failed");
        return ESP_FAIL;
    }

    code_json = cJSON_GetObjectItem(root, "code");
    token_json = cJSON_GetObjectItem(root, "tenant_access_token");
    expire_json = cJSON_GetObjectItem(root, "expire");
    if (!cJSON_IsNumber(code_json) || code_json->valueint != 0 ||
            !cJSON_IsString(token_json) || !token_json->valuestring) {
        ESP_LOGE(TAG, "Feishu token response invalid");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strlcpy(s_feishu.tenant_token, token_json->valuestring, sizeof(s_feishu.tenant_token));
    s_feishu.token_expire_time_ms = now_ms +
                                    (int64_t)(cJSON_IsNumber(expire_json) ? expire_json->valueint : 7200) * 1000LL;
    ESP_LOGI(TAG, "Feishu tenant token refreshed");
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_im_feishu_api_call(const char *url, const char *method, const char *body, char **out_response)
{
    char auth_header[CAP_IM_FEISHU_TOKEN_LEN + 16];
    int status = 0;
    esp_err_t err;

    if (!url || !method || !out_response) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_feishu_get_tenant_token();
    if (err != ESP_OK) {
        return err;
    }

    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_feishu.tenant_token);
    err = cap_im_feishu_http_json(url, method, auth_header, body, out_response, &status);
    if (err != ESP_OK) {
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Feishu API call HTTP %d url=%s", status, url);
        free(*out_response);
        *out_response = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t cap_im_feishu_validate_api_success_response(const char *response)
{
    cJSON *root = NULL;
    cJSON *code_json = NULL;
    esp_err_t err = ESP_FAIL;

    if (!response) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(response);
    if (!root) {
        return ESP_FAIL;
    }

    code_json = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code_json) && code_json->valueint == 0) {
        err = ESP_OK;
    }
    cJSON_Delete(root);
    return err;
}

static bool cap_im_feishu_parse_query_param(const char *url, const char *key, char *out, size_t out_size)
{
    const char *q = NULL;
    size_t key_len = 0;

    if (!url || !key || !out || out_size == 0) {
        return false;
    }

    q = strchr(url, '?');
    if (!q) {
        return false;
    }
    q++;
    key_len = strlen(key);

    while (*q) {
        const char *eq = strchr(q, '=');
        const char *amp = NULL;
        size_t name_len = 0;
        size_t value_len = 0;
        size_t copy_len = 0;

        if (!eq) {
            break;
        }
        amp = strchr(eq + 1, '&');
        name_len = (size_t)(eq - q);
        if (name_len == key_len && strncmp(q, key, key_len) == 0) {
            value_len = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
            copy_len = value_len < out_size - 1 ? value_len : out_size - 1;
            memcpy(out, eq + 1, copy_len);
            out[copy_len] = '\0';
            return true;
        }
        if (!amp) {
            break;
        }
        q = amp + 1;
    }

    return false;
}

static const char *cap_im_feishu_basename(const char *path)
{
    const char *slash = NULL;
    const char *backslash = NULL;

    if (!path || !path[0]) {
        return "";
    }

    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) {
        slash = backslash;
    }
    return slash ? slash + 1 : path;
}

static const char *cap_im_feishu_guess_upload_mime(const char *path, bool is_image)
{
    const char *extension = NULL;

    if (!path) {
        return is_image ? "image/jpeg" : "application/octet-stream";
    }

    extension = strrchr(path, '.');
    if (!extension || !extension[1]) {
        return is_image ? "image/jpeg" : "application/octet-stream";
    }

    extension++;
    if (strcasecmp(extension, "jpg") == 0 || strcasecmp(extension, "jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(extension, "png") == 0) {
        return "image/png";
    }
    if (strcasecmp(extension, "gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(extension, "webp") == 0) {
        return "image/webp";
    }
    if (strcasecmp(extension, "txt") == 0) {
        return "text/plain";
    }
    if (strcasecmp(extension, "json") == 0) {
        return "application/json";
    }
    if (strcasecmp(extension, "pdf") == 0) {
        return "application/pdf";
    }

    return is_image ? "image/jpeg" : "application/octet-stream";
}

static esp_err_t cap_im_feishu_http_client_write_all(esp_http_client_handle_t client,
                                                     const char *buf,
                                                     size_t len)
{
    size_t offset = 0;

    if (!client || (!buf && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    while (offset < len) {
        int written = esp_http_client_write(client, buf + offset, (int)(len - offset));

        if (written <= 0) {
            return ESP_FAIL;
        }
        offset += (size_t)written;
    }

    return ESP_OK;
}

static esp_err_t cap_im_feishu_stream_file_to_http_client(esp_http_client_handle_t client, FILE *file)
{
    char buf[2048];

    if (!client || !file) {
        return ESP_ERR_INVALID_ARG;
    }

    while (!feof(file)) {
        size_t read_len = fread(buf, 1, sizeof(buf), file);

        if (read_len == 0) {
            if (ferror(file)) {
                return ESP_FAIL;
            }
            break;
        }
        if (cap_im_feishu_http_client_write_all(client, buf, read_len) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static esp_err_t cap_im_feishu_pull_ws_config(void)
{
    cJSON *body = NULL;
    char *json_str = NULL;
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *code_json = NULL;
    cJSON *data_json = NULL;
    cJSON *url_json = NULL;
    cJSON *config_json = NULL;
    char service_id[24] = {0};
    int status = 0;
    esp_err_t err;

    body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "AppID", s_feishu.app_id);
    cJSON_AddStringToObject(body, "AppSecret", s_feishu.app_secret);
    json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_feishu_http_json(CAP_IM_FEISHU_WS_CONFIG_URL, "POST", NULL, json_str, &response, &status);
    free(json_str);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Feishu WS config request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Feishu WS config HTTP %d", status);
        free(response);
        return ESP_FAIL;
    }

    root = cJSON_Parse(response);
    free(response);
    if (!root) {
        ESP_LOGE(TAG, "Feishu WS config parse failed");
        return ESP_FAIL;
    }

    code_json = cJSON_GetObjectItem(root, "code");
    data_json = cJSON_GetObjectItem(root, "data");
    url_json = cJSON_IsObject(data_json) ? cJSON_GetObjectItem(data_json, "URL") : NULL;
    config_json = cJSON_IsObject(data_json) ? cJSON_GetObjectItem(data_json, "ClientConfig") : NULL;
    if (!cJSON_IsNumber(code_json) || code_json->valueint != 0 ||
            !cJSON_IsString(url_json) || !url_json->valuestring) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Feishu WS config response invalid");
        return ESP_FAIL;
    }

    strlcpy(s_feishu.ws_url, url_json->valuestring, sizeof(s_feishu.ws_url));
    if (cap_im_feishu_parse_query_param(s_feishu.ws_url, "service_id", service_id, sizeof(service_id))) {
        s_feishu.ws_service_id = atoi(service_id);
    }
    if (cJSON_IsObject(config_json)) {
        cJSON *ping_json = cJSON_GetObjectItem(config_json, "PingInterval");
        cJSON *reconnect_json = cJSON_GetObjectItem(config_json, "ReconnectInterval");
        cJSON *nonce_json = cJSON_GetObjectItem(config_json, "ReconnectNonce");

        if (cJSON_IsNumber(ping_json)) {
            s_feishu.ws_ping_interval_ms = ping_json->valueint * 1000;
        }
        if (cJSON_IsNumber(reconnect_json)) {
            s_feishu.ws_reconnect_interval_ms = reconnect_json->valueint * 1000;
        }
        if (cJSON_IsNumber(nonce_json)) {
            s_feishu.ws_reconnect_nonce_ms = nonce_json->valueint * 1000;
        }
    }

    ESP_LOGI(TAG, "Feishu WS endpoint ready service_id=%d", s_feishu.ws_service_id);
    cJSON_Delete(root);
    return ESP_OK;
}

static bool cap_im_feishu_pb_read_varint(const uint8_t *buf, size_t len, size_t *pos, uint64_t *out)
{
    uint64_t value = 0;
    int shift = 0;

    while (*pos < len && shift <= 63) {
        uint8_t byte = buf[(*pos)++];

        value |= ((uint64_t)(byte & 0x7F)) << shift;
        if ((byte & 0x80) == 0) {
            *out = value;
            return true;
        }
        shift += 7;
    }

    return false;
}

static bool cap_im_feishu_pb_skip_field(const uint8_t *buf, size_t len, size_t *pos, uint8_t wire_type)
{
    uint64_t size = 0;

    switch (wire_type) {
    case 0:
        return cap_im_feishu_pb_read_varint(buf, len, pos, &size);
    case 1:
        if (*pos + 8 > len) {
            return false;
        }
        *pos += 8;
        return true;
    case 2:
        if (!cap_im_feishu_pb_read_varint(buf, len, pos, &size)) {
            return false;
        }
        if (*pos + (size_t)size > len) {
            return false;
        }
        *pos += (size_t)size;
        return true;
    case 5:
        if (*pos + 4 > len) {
            return false;
        }
        *pos += 4;
        return true;
    default:
        return false;
    }
}

static bool cap_im_feishu_pb_parse_header(const uint8_t *buf,
                                          size_t len,
                                          cap_im_feishu_ws_header_t *header)
{
    size_t pos = 0;

    memset(header, 0, sizeof(*header));
    while (pos < len) {
        uint64_t tag = 0;
        uint64_t size = 0;
        uint32_t field = 0;
        uint8_t wire_type = 0;
        size_t copy_len = 0;

        if (!cap_im_feishu_pb_read_varint(buf, len, &pos, &tag)) {
            return false;
        }
        field = (uint32_t)(tag >> 3);
        wire_type = (uint8_t)(tag & 0x07);
        if (wire_type != 2) {
            if (!cap_im_feishu_pb_skip_field(buf, len, &pos, wire_type)) {
                return false;
            }
            continue;
        }
        if (!cap_im_feishu_pb_read_varint(buf, len, &pos, &size) || pos + (size_t)size > len) {
            return false;
        }
        if (field == 1) {
            copy_len = (size_t)size < sizeof(header->key) - 1 ? (size_t)size : sizeof(header->key) - 1;
            memcpy(header->key, buf + pos, copy_len);
            header->key[copy_len] = '\0';
        } else if (field == 2) {
            copy_len = (size_t)size < sizeof(header->value) - 1 ? (size_t)size : sizeof(header->value) - 1;
            memcpy(header->value, buf + pos, copy_len);
            header->value[copy_len] = '\0';
        }
        pos += (size_t)size;
    }

    return true;
}

static bool cap_im_feishu_pb_parse_frame(const uint8_t *buf,
                                         size_t len,
                                         cap_im_feishu_ws_frame_t *frame)
{
    size_t pos = 0;

    memset(frame, 0, sizeof(*frame));
    while (pos < len) {
        uint64_t tag = 0;
        uint64_t value = 0;
        uint64_t size = 0;
        uint32_t field = 0;
        uint8_t wire_type = 0;

        if (!cap_im_feishu_pb_read_varint(buf, len, &pos, &tag)) {
            return false;
        }
        field = (uint32_t)(tag >> 3);
        wire_type = (uint8_t)(tag & 0x07);

        if (field == 1 && wire_type == 0) {
            if (!cap_im_feishu_pb_read_varint(buf, len, &pos, &frame->seq_id)) {
                return false;
            }
        } else if (field == 2 && wire_type == 0) {
            if (!cap_im_feishu_pb_read_varint(buf, len, &pos, &frame->log_id)) {
                return false;
            }
        } else if (field == 3 && wire_type == 0) {
            if (!cap_im_feishu_pb_read_varint(buf, len, &pos, &value)) {
                return false;
            }
            frame->service = (int32_t)value;
        } else if (field == 4 && wire_type == 0) {
            if (!cap_im_feishu_pb_read_varint(buf, len, &pos, &value)) {
                return false;
            }
            frame->method = (int32_t)value;
        } else if (field == 5 && wire_type == 2) {
            if (!cap_im_feishu_pb_read_varint(buf, len, &pos, &size) || pos + (size_t)size > len) {
                return false;
            }
            if (frame->header_count < CAP_IM_FEISHU_MAX_HEADERS) {
                cap_im_feishu_pb_parse_header(buf + pos, (size_t)size, &frame->headers[frame->header_count++]);
            }
            pos += (size_t)size;
        } else if (field == 8 && wire_type == 2) {
            if (!cap_im_feishu_pb_read_varint(buf, len, &pos, &size) || pos + (size_t)size > len) {
                return false;
            }
            frame->payload = buf + pos;
            frame->payload_len = (size_t)size;
            pos += (size_t)size;
        } else {
            if (!cap_im_feishu_pb_skip_field(buf, len, &pos, wire_type)) {
                return false;
            }
        }
    }

    return true;
}

static const char *cap_im_feishu_ws_header_value(const cap_im_feishu_ws_frame_t *frame, const char *key)
{
    size_t i;

    if (!frame || !key) {
        return NULL;
    }

    for (i = 0; i < frame->header_count; i++) {
        if (strcmp(frame->headers[i].key, key) == 0) {
            return frame->headers[i].value;
        }
    }

    return NULL;
}

static bool cap_im_feishu_pb_write_varint(uint8_t *buf, size_t cap, size_t *pos, uint64_t value)
{
    do {
        uint8_t byte = (uint8_t)(value & 0x7F);

        if (*pos >= cap) {
            return false;
        }
        value >>= 7;
        if (value) {
            byte |= 0x80;
        }
        buf[(*pos)++] = byte;
    } while (value);

    return true;
}

static bool cap_im_feishu_pb_write_bytes(uint8_t *buf,
                                         size_t cap,
                                         size_t *pos,
                                         uint32_t field,
                                         const uint8_t *data,
                                         size_t len)
{
    if (!cap_im_feishu_pb_write_varint(buf, cap, pos, ((uint64_t)field << 3) | 2) ||
            !cap_im_feishu_pb_write_varint(buf, cap, pos, len) ||
            *pos + len > cap) {
        return false;
    }

    memcpy(buf + *pos, data, len);
    *pos += len;
    return true;
}

static bool cap_im_feishu_pb_write_string(uint8_t *buf,
                                          size_t cap,
                                          size_t *pos,
                                          uint32_t field,
                                          const char *text)
{
    return cap_im_feishu_pb_write_bytes(buf,
                                        cap,
                                        pos,
                                        field,
                                        (const uint8_t *)(text ? text : ""),
                                        strlen(text ? text : ""));
}

static bool cap_im_feishu_pb_write_header(uint8_t *buf,
                                          size_t cap,
                                          size_t *pos,
                                          const cap_im_feishu_ws_header_t *header)
{
    uint8_t tmp[256];
    size_t tmp_pos = 0;

    if (!header) {
        return false;
    }
    if (!cap_im_feishu_pb_write_string(tmp, sizeof(tmp), &tmp_pos, 1, header->key) ||
            !cap_im_feishu_pb_write_string(tmp, sizeof(tmp), &tmp_pos, 2, header->value)) {
        return false;
    }

    return cap_im_feishu_pb_write_bytes(buf, cap, pos, 5, tmp, tmp_pos);
}

static int cap_im_feishu_ws_send_frame(const cap_im_feishu_ws_frame_t *frame,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       int timeout_ms)
{
    uint8_t buf[1024];
    size_t pos = 0;
    size_t i = 0;

    if (!frame || !s_feishu.ws_client) {
        return -1;
    }

    if (!cap_im_feishu_pb_write_varint(buf, sizeof(buf), &pos, ((uint64_t)1 << 3)) ||
            !cap_im_feishu_pb_write_varint(buf, sizeof(buf), &pos, frame->seq_id) ||
            !cap_im_feishu_pb_write_varint(buf, sizeof(buf), &pos, ((uint64_t)2 << 3)) ||
            !cap_im_feishu_pb_write_varint(buf, sizeof(buf), &pos, frame->log_id) ||
            !cap_im_feishu_pb_write_varint(buf, sizeof(buf), &pos, ((uint64_t)3 << 3)) ||
            !cap_im_feishu_pb_write_varint(buf, sizeof(buf), &pos, frame->service) ||
            !cap_im_feishu_pb_write_varint(buf, sizeof(buf), &pos, ((uint64_t)4 << 3)) ||
            !cap_im_feishu_pb_write_varint(buf, sizeof(buf), &pos, frame->method)) {
        return -1;
    }

    for (i = 0; i < frame->header_count; i++) {
        if (!cap_im_feishu_pb_write_header(buf, sizeof(buf), &pos, &frame->headers[i])) {
            return -1;
        }
    }
    if (payload && payload_len > 0 &&
            !cap_im_feishu_pb_write_bytes(buf, sizeof(buf), &pos, 8, payload, payload_len)) {
        return -1;
    }

    return esp_websocket_client_send_bin(s_feishu.ws_client, (const char *)buf, pos, timeout_ms);
}

static esp_err_t cap_im_feishu_publish_inbound_text(const char *chat_id,
                                                    const char *sender_id,
                                                    const char *message_id,
                                                    const char *content)
{
    if (!content || !content[0]) {
        return ESP_OK;
    }

    return claw_event_router_publish_message("feishu_gateway",
                                             "feishu",
                                             chat_id,
                                             content,
                                             sender_id,
                                             message_id);
}

static esp_err_t cap_im_feishu_publish_attachment_event(const char *chat_id,
                                                        const char *sender_id,
                                                        const char *message_id,
                                                        const char *content_type,
                                                        const char *payload_json)
{
    claw_event_t event = {0};

    if (!chat_id || !message_id || !content_type || !payload_json) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(event.source_cap, "feishu_gateway", sizeof(event.source_cap));
    strlcpy(event.event_type, "attachment_saved", sizeof(event.event_type));
    strlcpy(event.source_channel, "feishu", sizeof(event.source_channel));
    strlcpy(event.chat_id, chat_id, sizeof(event.chat_id));
    strlcpy(event.content_type, content_type, sizeof(event.content_type));
    strlcpy(event.message_id, message_id, sizeof(event.message_id));
    if (sender_id && sender_id[0]) {
        strlcpy(event.sender_id, sender_id, sizeof(event.sender_id));
    }
    snprintf(event.event_id, sizeof(event.event_id), "feishu-attach-%" PRId64, cap_im_feishu_now_ms());
    event.timestamp_ms = cap_im_feishu_now_ms();
    event.session_policy = CLAW_EVENT_SESSION_POLICY_CHAT;
    event.text = "";
    event.payload_json = (char *)payload_json;
    return claw_event_router_publish(&event);
}

static char *cap_im_feishu_strdup_or_empty(const char *value)
{
    return strdup(value ? value : "");
}

static void cap_im_feishu_free_attachment_job(cap_im_feishu_attachment_job_t *job)
{
    if (!job) {
        return;
    }

    free(job->chat_id);
    free(job->sender_id);
    free(job->message_id);
    free(job->attachment_kind);
    free(job->resource_key);
    free(job->original_filename);
    free(job->content_json);
    free(job->content_type);
    free(job);
}

static cap_im_feishu_attachment_job_t *cap_im_feishu_make_attachment_job(const char *chat_id,
                                                                         const char *sender_id,
                                                                         const char *message_id,
                                                                         const char *attachment_kind,
                                                                         const char *resource_key,
                                                                         const char *original_filename,
                                                                         const char *content_json,
                                                                         const char *content_type)
{
    cap_im_feishu_attachment_job_t *job = calloc(1, sizeof(*job));

    if (!job) {
        return NULL;
    }

    job->chat_id = cap_im_feishu_strdup_or_empty(chat_id);
    job->sender_id = cap_im_feishu_strdup_or_empty(sender_id);
    job->message_id = cap_im_feishu_strdup_or_empty(message_id);
    job->attachment_kind = cap_im_feishu_strdup_or_empty(attachment_kind);
    job->resource_key = cap_im_feishu_strdup_or_empty(resource_key);
    job->original_filename = cap_im_feishu_strdup_or_empty(original_filename);
    job->content_json = cap_im_feishu_strdup_or_empty(content_json);
    job->content_type = cap_im_feishu_strdup_or_empty(content_type);
    if (!job->chat_id || !job->sender_id || !job->message_id || !job->attachment_kind ||
            !job->resource_key || !job->original_filename || !job->content_json || !job->content_type) {
        cap_im_feishu_free_attachment_job(job);
        return NULL;
    }

    return job;
}

static esp_err_t cap_im_feishu_ensure_dir(const char *path)
{
    struct stat st = {0};

    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_im_feishu_ensure_parent_dirs(const char *path)
{
    char dir[CAP_IM_FEISHU_PATH_LEN];
    char *cursor = NULL;
    char *slash = NULL;
    char *create_from = NULL;
    struct stat st = {0};

    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(dir, path, sizeof(dir));
    slash = strrchr(dir, '/');
    if (!slash) {
        return ESP_OK;
    }
    *slash = '\0';

    if (dir[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    for (cursor = dir + 1; *cursor; cursor++) {
        if (*cursor != '/') {
            continue;
        }

        *cursor = '\0';
        if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            create_from = cursor + 1;
        }
        *cursor = '/';
    }

    if (!create_from) {
        return ESP_FAIL;
    }

    for (cursor = create_from; *cursor; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (cap_im_feishu_ensure_dir(dir) != ESP_OK) {
                *cursor = '/';
                return ESP_FAIL;
            }
            *cursor = '/';
        }
    }

    return cap_im_feishu_ensure_dir(dir);
}

static esp_err_t cap_im_feishu_save_attachment_metadata(const char *chat_id,
                                                        const char *sender_id,
                                                        const char *message_id,
                                                        const char *message_type,
                                                        const char *content_json)
{
    char saved_dir[CAP_IM_FEISHU_PATH_LEN];
    char saved_name[CAP_IM_FEISHU_NAME_LEN];
    char saved_path[CAP_IM_FEISHU_PATH_LEN];
    char *payload_json = NULL;
    esp_err_t err;

    if (!s_feishu.enable_inbound_attachments || !s_feishu.attachment_root_dir[0] ||
            !chat_id || !message_id || !message_type || !content_json) {
        return ESP_ERR_INVALID_STATE;
    }

    err = cap_im_attachment_build_saved_paths(s_feishu.attachment_root_dir,
                                              "feishu",
                                              chat_id,
                                              message_id,
                                              message_type,
                                              ".json",
                                              saved_dir,
                                              sizeof(saved_dir),
                                              saved_name,
                                              sizeof(saved_name),
                                              saved_path,
                                              sizeof(saved_path));
    if (err != ESP_OK) {
        return err;
    }

    err = cap_im_attachment_save_buffer_to_file(TAG,
                                                saved_path,
                                                (const unsigned char *)content_json,
                                                strlen(content_json));
    if (err != ESP_OK) {
        return err;
    }

    payload_json = cap_im_attachment_build_payload_json(
    &(cap_im_attachment_payload_config_t) {
        .platform = "feishu",
        .attachment_kind = message_type,
        .saved_path = saved_path,
        .saved_dir = saved_dir,
        .saved_name = saved_name,
        .original_filename = NULL,
        .mime = "application/json",
        .caption = NULL,
        .source_key = "message_type",
        .source_value = message_type,
        .size_bytes = strlen(content_json),
        .saved_at_ms = cap_im_feishu_now_ms(),
    });
    if (!payload_json) {
        ESP_LOGW(TAG, "Feishu metadata payload build failed message=%s path=%s", message_id, saved_path);
        return ESP_OK;
    }

    err = cap_im_feishu_publish_attachment_event(chat_id,
                                                 sender_id,
                                                 message_id,
                                                 message_type,
                                                 payload_json);
    free(payload_json);
    return err;
}

static esp_err_t cap_im_feishu_extract_attachment_fields(const char *message_type,
                                                         const char *content_json,
                                                         char **out_resource_key,
                                                         char **out_original_filename)
{
    cJSON *root = NULL;
    cJSON *resource_json = NULL;
    cJSON *name_json = NULL;

    if (out_resource_key) {
        *out_resource_key = NULL;
    }
    if (out_original_filename) {
        *out_original_filename = NULL;
    }
    if (!message_type || !content_json || !out_resource_key || !out_original_filename) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(content_json);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (strcmp(message_type, "image") == 0) {
        resource_json = cJSON_GetObjectItem(root, "image_key");
        name_json = cJSON_GetObjectItem(root, "file_name");
    } else if (strcmp(message_type, "file") == 0) {
        resource_json = cJSON_GetObjectItem(root, "file_key");
        name_json = cJSON_GetObjectItem(root, "file_name");
    }

    if (!cJSON_IsString(resource_json) || !resource_json->valuestring || !resource_json->valuestring[0]) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    *out_resource_key = strdup(resource_json->valuestring);
    if (cJSON_IsString(name_json) && name_json->valuestring && name_json->valuestring[0]) {
        *out_original_filename = strdup(name_json->valuestring);
    } else {
        *out_original_filename = strdup("");
    }
    cJSON_Delete(root);
    if (!*out_resource_key || !*out_original_filename) {
        free(*out_resource_key);
        free(*out_original_filename);
        *out_resource_key = NULL;
        *out_original_filename = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t cap_im_feishu_download_attachment(const char *message_id,
                                                   const char *resource_key,
                                                   const char *resource_type,
                                                   const char *saved_path,
                                                   char *mime_buf,
                                                   size_t mime_buf_size,
                                                   size_t *out_bytes)
{
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    char auth_header[CAP_IM_FEISHU_TOKEN_LEN + 16];
    char url[CAP_IM_FEISHU_URL_LEN];
    FILE *file = NULL;
    char read_buf[2048];
    size_t total_bytes = 0;
    int content_length = 0;
    int status = 0;
    esp_err_t err;

    if (mime_buf && mime_buf_size > 0) {
        mime_buf[0] = '\0';
    }
    if (out_bytes) {
        *out_bytes = 0;
    }
    if (!message_id || !resource_key || !resource_type || !saved_path) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_feishu_get_tenant_token();
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Feishu attachment token fetch failed message=%s kind=%s err=%s",
                 message_id,
                 resource_type,
                 esp_err_to_name(err));
        return err;
    }

    snprintf(url,
             sizeof(url),
             CAP_IM_FEISHU_API_BASE "/im/v1/messages/%s/resources/%s?type=%s",
             message_id,
             resource_key,
             resource_type);
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_feishu.tenant_token);

    config.url = url;
    config.timeout_ms = 30000;
    config.buffer_size = sizeof(read_buf);
    config.buffer_size_tx = 1024;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Feishu attachment HTTP client init failed message=%s", message_id);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Authorization", auth_header);

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Feishu attachment open failed message=%s kind=%s err=%s url=%s",
                 message_id,
                 resource_type,
                 esp_err_to_name(err),
                 url);
        esp_http_client_cleanup(client);
        return err;
    }

    content_length = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        char error_buf[192] = {0};
        int error_len = esp_http_client_read(client, error_buf, sizeof(error_buf) - 1);

        if (error_len > 0) {
            error_buf[error_len] = '\0';
        } else {
            error_buf[0] = '\0';
        }
        ESP_LOGW(TAG,
                 "Feishu attachment HTTP error message=%s kind=%s status=%d body=%s",
                 message_id,
                 resource_type,
                 status,
                 error_buf[0] ? error_buf : "(empty)");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (content_length > 0 &&
            (size_t)content_length > (s_feishu.max_inbound_file_bytes ? s_feishu.max_inbound_file_bytes :
                                      (2 * 1024 * 1024))) {
        ESP_LOGW(TAG,
                 "Feishu attachment too large before read message=%s kind=%s content_length=%d limit=%u",
                 message_id,
                 resource_type,
                 content_length,
                 (unsigned)(s_feishu.max_inbound_file_bytes ? s_feishu.max_inbound_file_bytes :
                            (2 * 1024 * 1024)));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    err = cap_im_feishu_ensure_parent_dirs(saved_path);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Feishu attachment ensure dir failed message=%s path=%s err=%s",
                 message_id,
                 saved_path,
                 esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    file = fopen(saved_path, "wb");
    if (!file) {
        ESP_LOGW(TAG, "Feishu attachment open file failed path=%s errno=%d", saved_path, errno);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    while (1) {
        int read_len = esp_http_client_read(client, read_buf, sizeof(read_buf));

        if (read_len < 0) {
            ESP_LOGW(TAG,
                     "Feishu attachment read failed message=%s kind=%s after=%u",
                     message_id,
                     resource_type,
                     (unsigned)total_bytes);
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            err = ESP_OK;
            break;
        }
        total_bytes += (size_t)read_len;
        if (total_bytes > (s_feishu.max_inbound_file_bytes ? s_feishu.max_inbound_file_bytes :
                           (2 * 1024 * 1024))) {
            ESP_LOGW(TAG,
                     "Feishu attachment exceeded limit while reading message=%s kind=%s bytes=%u limit=%u",
                     message_id,
                     resource_type,
                     (unsigned)total_bytes,
                     (unsigned)(s_feishu.max_inbound_file_bytes ? s_feishu.max_inbound_file_bytes :
                                (2 * 1024 * 1024)));
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (fwrite(read_buf, 1, (size_t)read_len, file) != (size_t)read_len) {
            ESP_LOGW(TAG,
                     "Feishu attachment fwrite failed path=%s errno=%d bytes=%u",
                     saved_path,
                     errno,
                     (unsigned)total_bytes);
            err = ESP_FAIL;
            break;
        }
    }

    fclose(file);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || total_bytes == 0) {
        ESP_LOGW(TAG,
                 "Feishu attachment download failed final message=%s kind=%s err=%s bytes=%u",
                 message_id,
                 resource_type,
                 esp_err_to_name(err != ESP_OK ? err : ESP_FAIL),
                 (unsigned)total_bytes);
        remove(saved_path);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    if (mime_buf && mime_buf_size > 0 && !mime_buf[0]) {
        if (strcmp(resource_type, "image") == 0) {
            strlcpy(mime_buf, "image/jpeg", mime_buf_size);
        } else if (strcmp(resource_type, "file") == 0) {
            strlcpy(mime_buf, "application/octet-stream", mime_buf_size);
        }
    }

    if (out_bytes) {
        *out_bytes = total_bytes;
    }
    return ESP_OK;
}

static esp_err_t cap_im_feishu_save_attachment(const char *chat_id,
                                               const char *sender_id,
                                               const char *message_id,
                                               const char *attachment_kind,
                                               const char *resource_key,
                                               const char *original_filename,
                                               const char *content_json,
                                               const char *content_type)
{
    char saved_dir[CAP_IM_FEISHU_PATH_LEN];
    char saved_name[CAP_IM_FEISHU_NAME_LEN];
    char saved_path[CAP_IM_FEISHU_PATH_LEN];
    char mime[64] = {0};
    const char *extension = NULL;
    char *payload_json = NULL;
    size_t bytes = 0;
    esp_err_t err;

    if (!chat_id || !message_id || !attachment_kind || !resource_key || !content_type) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((!original_filename || !original_filename[0]) && strcmp(attachment_kind, "image") == 0) {
        extension = ".jpg";
    } else {
        extension = cap_im_attachment_guess_extension(NULL,
                                                      original_filename && original_filename[0] ? original_filename : NULL,
                                                      NULL);
    }
    err = cap_im_attachment_build_saved_paths(s_feishu.attachment_root_dir,
                                              "feishu",
                                              chat_id,
                                              message_id,
                                              attachment_kind,
                                              extension,
                                              saved_dir,
                                              sizeof(saved_dir),
                                              saved_name,
                                              sizeof(saved_name),
                                              saved_path,
                                              sizeof(saved_path));
    if (err != ESP_OK) {
        return err;
    }

    if (original_filename && original_filename[0]) {
        const char *original_basename = cap_im_feishu_basename(original_filename);
        uint64_t message_hash = cap_im_feishu_fnv1a64(message_id);
        int written;

        if (original_basename[0]) {
            written = snprintf(saved_name,
                               sizeof(saved_name),
                               "feishu_%08" PRIx32 "_%s",
                               (uint32_t)message_hash,
                               original_basename);
            if (written < 0 || (size_t)written >= sizeof(saved_name)) {
                return ESP_ERR_INVALID_SIZE;
            }

            written = snprintf(saved_path, sizeof(saved_path), "%s/%s", saved_dir, saved_name);
            if (written < 0 || (size_t)written >= sizeof(saved_path)) {
                return ESP_ERR_INVALID_SIZE;
            }
        }
    }

    err = cap_im_feishu_download_attachment(message_id,
                                            resource_key,
                                            attachment_kind,
                                            saved_path,
                                            mime,
                                            sizeof(mime),
                                            &bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Feishu attachment download failed message=%s kind=%s err=%s, fallback to metadata",
                 message_id,
                 attachment_kind,
                 esp_err_to_name(err));
        return cap_im_feishu_save_attachment_metadata(chat_id,
                                                      sender_id,
                                                      message_id,
                                                      attachment_kind,
                                                      content_json);
    }

    payload_json = cap_im_attachment_build_payload_json(
    &(cap_im_attachment_payload_config_t) {
        .platform = "feishu",
        .attachment_kind = attachment_kind,
        .saved_path = saved_path,
        .saved_dir = saved_dir,
        .saved_name = saved_name,
        .original_filename = original_filename,
        .mime = mime[0] ? mime : NULL,
        .caption = NULL,
        .source_key = "resource_key",
        .source_value = resource_key,
        .size_bytes = bytes,
        .saved_at_ms = cap_im_feishu_now_ms(),
    });
    if (!payload_json) {
        ESP_LOGW(TAG, "Feishu attachment payload build failed message=%s path=%s", message_id, saved_path);
        return ESP_OK;
    }

    err = cap_im_feishu_publish_attachment_event(chat_id,
                                                 sender_id,
                                                 message_id,
                                                 content_type,
                                                 payload_json);
    free(payload_json);
    return err;
}

static void cap_im_feishu_attachment_task(void *arg)
{
    cap_im_feishu_attachment_job_t *job = NULL;

    (void)arg;

    while (1) {
        if (xQueueReceive(s_feishu.attachment_queue, &job, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (job) {
                esp_err_t err = cap_im_feishu_save_attachment(job->chat_id,
                                                              job->sender_id,
                                                              job->message_id,
                                                              job->attachment_kind,
                                                              job->resource_key,
                                                              job->original_filename,
                                                              job->content_json,
                                                              job->content_type);

                if (err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "Feishu attachment save failed message=%s kind=%s err=%s",
                             job->message_id,
                             job->attachment_kind,
                             esp_err_to_name(err));
                }
                cap_im_feishu_free_attachment_job(job);
            }
            continue;
        }

        if (s_feishu.stop_requested) {
            break;
        }
    }

    s_feishu.attachment_task = NULL;
    claw_task_delete(NULL);
}

static void cap_im_feishu_queue_attachment(const char *chat_id,
                                           const char *sender_id,
                                           const char *message_id,
                                           const char *attachment_kind,
                                           const char *content_json)
{
    char *resource_key = NULL;
    char *original_filename = NULL;
    cap_im_feishu_attachment_job_t *job = NULL;
    esp_err_t err;

    if (!s_feishu.enable_inbound_attachments || !s_feishu.attachment_queue || !content_json || !content_json[0]) {
        return;
    }

    err = cap_im_feishu_extract_attachment_fields(attachment_kind,
                                                  content_json,
                                                  &resource_key,
                                                  &original_filename);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Feishu attachment parse failed message=%s kind=%s err=%s",
                 message_id,
                 attachment_kind,
                 esp_err_to_name(err));
        return;
    }

    job = cap_im_feishu_make_attachment_job(chat_id,
                                            sender_id,
                                            message_id,
                                            attachment_kind,
                                            resource_key,
                                            original_filename,
                                            content_json,
                                            attachment_kind);
    free(resource_key);
    free(original_filename);
    if (!job) {
        ESP_LOGW(TAG, "Feishu attachment alloc failed message=%s", message_id);
        return;
    }

    if (xQueueSend(s_feishu.attachment_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Feishu attachment queue full message=%s kind=%s", message_id, attachment_kind);
        cap_im_feishu_free_attachment_job(job);
    }
}

static char *cap_im_feishu_build_attachment_content_json(const char *attachment_kind,
                                                         const char *resource_key,
                                                         const char *original_filename)
{
    cJSON *root = NULL;
    char *json = NULL;

    if (!attachment_kind || !resource_key || !resource_key[0]) {
        return NULL;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    if (strcmp(attachment_kind, "image") == 0) {
        cJSON_AddStringToObject(root, "image_key", resource_key);
        if (original_filename && original_filename[0]) {
            cJSON_AddStringToObject(root, "file_name", original_filename);
        }
    } else {
        cJSON_AddStringToObject(root, "file_key", resource_key);
        if (original_filename && original_filename[0]) {
            cJSON_AddStringToObject(root, "file_name", original_filename);
        }
    }

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void cap_im_feishu_queue_post_attachment(const char *chat_id,
                                                const char *sender_id,
                                                const char *message_id,
                                                const char *attachment_kind,
                                                const char *resource_key,
                                                const char *original_filename)
{
    char *content_json = NULL;

    if (!resource_key || !resource_key[0]) {
        return;
    }

    content_json = cap_im_feishu_build_attachment_content_json(attachment_kind,
                                                               resource_key,
                                                               original_filename);
    if (!content_json) {
        ESP_LOGW(TAG, "Feishu post attachment json build failed message=%s", message_id);
        return;
    }
    cap_im_feishu_queue_attachment(chat_id,
                                   sender_id,
                                   message_id,
                                   attachment_kind,
                                   content_json);
    free(content_json);
}

static char *cap_im_feishu_extract_text(const char *content_json)
{
    cJSON *root = NULL;
    cJSON *text_json = NULL;
    char *copy = NULL;

    if (!content_json || !content_json[0]) {
        return NULL;
    }

    root = cJSON_Parse(content_json);
    if (!root) {
        return NULL;
    }
    text_json = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text_json) && text_json->valuestring && text_json->valuestring[0]) {
        copy = strdup(text_json->valuestring);
    }
    cJSON_Delete(root);
    return copy;
}

static bool cap_im_feishu_post_has_style(cJSON *style, const char *name)
{
    cJSON *item = NULL;

    if (!cJSON_IsArray(style) || !name) {
        return false;
    }

    cJSON_ArrayForEach(item, style) {
        if (cJSON_IsString(item) && item->valuestring && strcmp(item->valuestring, name) == 0) {
            return true;
        }
    }

    return false;
}

static cJSON *cap_im_feishu_post_select_body(cJSON *root)
{
    static const char *locale_keys[] = {"zh_cn", "en_us", "ja_jp"};
    size_t i = 0;
    cJSON *body = NULL;
    cJSON *content = NULL;

    if (!cJSON_IsObject(root)) {
        return NULL;
    }

    content = cJSON_GetObjectItem(root, "content");
    if (cJSON_IsArray(content)) {
        return root;
    }

    for (i = 0; i < sizeof(locale_keys) / sizeof(locale_keys[0]); i++) {
        body = cJSON_GetObjectItem(root, locale_keys[i]);
        content = cJSON_IsObject(body) ? cJSON_GetObjectItem(body, "content") : NULL;
        if (cJSON_IsArray(content)) {
            return body;
        }
    }

    cJSON_ArrayForEach(body, root) {
        content = cJSON_IsObject(body) ? cJSON_GetObjectItem(body, "content") : NULL;
        if (cJSON_IsArray(content)) {
            return body;
        }
    }

    return NULL;
}

static esp_err_t cap_im_feishu_post_append_styled_text(cap_im_feishu_resp_t *markdown,
                                                       const char *text,
                                                       cJSON *style)
{
    bool bold = cap_im_feishu_post_has_style(style, "bold");
    bool italic = cap_im_feishu_post_has_style(style, "italic");
    bool underline = cap_im_feishu_post_has_style(style, "underline");
    bool line_through = cap_im_feishu_post_has_style(style, "lineThrough");
    bool code_inline = cap_im_feishu_post_has_style(style, "codeInline");
    esp_err_t err;

    if (!text) {
        return ESP_OK;
    }

    if (bold && (err = cap_im_feishu_resp_append(markdown, "**", 2)) != ESP_OK) {
        return err;
    }
    if (italic && (err = cap_im_feishu_resp_append(markdown, "*", 1)) != ESP_OK) {
        return err;
    }
    if (underline && (err = cap_im_feishu_resp_append(markdown, "<u>", 3)) != ESP_OK) {
        return err;
    }
    if (line_through && (err = cap_im_feishu_resp_append(markdown, "~~", 2)) != ESP_OK) {
        return err;
    }
    if (code_inline && (err = cap_im_feishu_resp_append(markdown, "`", 1)) != ESP_OK) {
        return err;
    }

    err = cap_im_feishu_resp_append(markdown, text, strlen(text));
    if (err != ESP_OK) {
        return err;
    }

    if (code_inline && (err = cap_im_feishu_resp_append(markdown, "`", 1)) != ESP_OK) {
        return err;
    }
    if (line_through && (err = cap_im_feishu_resp_append(markdown, "~~", 2)) != ESP_OK) {
        return err;
    }
    if (underline && (err = cap_im_feishu_resp_append(markdown, "</u>", 4)) != ESP_OK) {
        return err;
    }
    if (italic && (err = cap_im_feishu_resp_append(markdown, "*", 1)) != ESP_OK) {
        return err;
    }
    if (bold && (err = cap_im_feishu_resp_append(markdown, "**", 2)) != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

static esp_err_t cap_im_feishu_post_append_element(cap_im_feishu_resp_t *markdown,
                                                   cJSON *element,
                                                   const char *chat_id,
                                                   const char *sender_id,
                                                   const char *message_id)
{
    cJSON *tag_json = NULL;
    cJSON *text_json = NULL;
    cJSON *href_json = NULL;
    cJSON *style_json = NULL;
    cJSON *language_json = NULL;
    cJSON *image_key_json = NULL;
    cJSON *file_key_json = NULL;
    cJSON *name_json = NULL;
    cJSON *user_id_json = NULL;
    const char *tag = NULL;
    const char *text = NULL;
    const char *href = NULL;
    const char *key = NULL;
    esp_err_t err;

    if (!cJSON_IsObject(element)) {
        return ESP_OK;
    }

    tag_json = cJSON_GetObjectItem(element, "tag");
    tag = cJSON_IsString(tag_json) ? tag_json->valuestring : NULL;
    if (!tag) {
        return ESP_OK;
    }

    text_json = cJSON_GetObjectItem(element, "text");
    text = cJSON_IsString(text_json) && text_json->valuestring ? text_json->valuestring : "";
    style_json = cJSON_GetObjectItem(element, "style");

    if (strcmp(tag, "text") == 0) {
        return cap_im_feishu_post_append_styled_text(markdown, text, style_json);
    }

    if (strcmp(tag, "a") == 0) {
        href_json = cJSON_GetObjectItem(element, "href");
        href = cJSON_IsString(href_json) ? href_json->valuestring : NULL;
        if (!href || !href[0]) {
            return cap_im_feishu_post_append_styled_text(markdown, text, style_json);
        }
        if ((err = cap_im_feishu_resp_append(markdown, "[", 1)) != ESP_OK ||
                (err = cap_im_feishu_post_append_styled_text(markdown, text, style_json)) != ESP_OK ||
                (err = cap_im_feishu_resp_append(markdown, "](", 2)) != ESP_OK ||
                (err = cap_im_feishu_resp_append(markdown, href, strlen(href))) != ESP_OK ||
                (err = cap_im_feishu_resp_append(markdown, ")", 1)) != ESP_OK) {
            return err;
        }
        return ESP_OK;
    }

    if (strcmp(tag, "at") == 0) {
        user_id_json = cJSON_GetObjectItem(element, "user_id");
        name_json = cJSON_GetObjectItem(element, "user_name");
        if (cJSON_IsString(user_id_json) && user_id_json->valuestring &&
                strcmp(user_id_json->valuestring, "all") == 0) {
            return cap_im_feishu_resp_append(markdown, "@all", 4);
        }
        if (cJSON_IsString(name_json) && name_json->valuestring && name_json->valuestring[0]) {
            if ((err = cap_im_feishu_resp_append(markdown, "@", 1)) != ESP_OK) {
                return err;
            }
            return cap_im_feishu_resp_append(markdown, name_json->valuestring, strlen(name_json->valuestring));
        }
        if (cJSON_IsString(user_id_json) && user_id_json->valuestring && user_id_json->valuestring[0]) {
            if ((err = cap_im_feishu_resp_append(markdown, "@", 1)) != ESP_OK) {
                return err;
            }
            return cap_im_feishu_resp_append(markdown, user_id_json->valuestring, strlen(user_id_json->valuestring));
        }
        return cap_im_feishu_resp_append(markdown, "@unknown", 8);
    }

    if (strcmp(tag, "img") == 0) {
        char image_filename[CAP_IM_FEISHU_NAME_LEN];
        image_key_json = cJSON_GetObjectItem(element, "image_key");
        key = cJSON_IsString(image_key_json) ? image_key_json->valuestring : NULL;
        if (!key || !key[0]) {
            return ESP_OK;
        }
        snprintf(image_filename,
                 sizeof(image_filename),
                 "image_%08" PRIx32 ".jpg",
                 (uint32_t)cap_im_feishu_fnv1a64(key));
        cap_im_feishu_queue_post_attachment(chat_id, sender_id, message_id, "image", key, image_filename);
        if ((err = cap_im_feishu_resp_append(markdown,
                                             "![image](feishu:image:",
                                             sizeof("![image](feishu:image:") - 1)) != ESP_OK ||
                (err = cap_im_feishu_resp_append(markdown, key, strlen(key))) != ESP_OK ||
                (err = cap_im_feishu_resp_append(markdown, ")", 1)) != ESP_OK) {
            return err;
        }
        return ESP_OK;
    }

    if (strcmp(tag, "media") == 0 || strcmp(tag, "file") == 0) {
        file_key_json = cJSON_GetObjectItem(element, "file_key");
        key = cJSON_IsString(file_key_json) ? file_key_json->valuestring : NULL;
        name_json = cJSON_GetObjectItem(element, "file_name");
        text = cJSON_IsString(name_json) && name_json->valuestring && name_json->valuestring[0] ?
               name_json->valuestring : "file";
        if (!key || !key[0]) {
            return cap_im_feishu_resp_append(markdown, text, strlen(text));
        }
        cap_im_feishu_queue_post_attachment(chat_id, sender_id, message_id, "file", key, text);
        if ((err = cap_im_feishu_resp_append(markdown, "[", 1)) != ESP_OK ||
                (err = cap_im_feishu_resp_append(markdown, text, strlen(text))) != ESP_OK ||
                (err = cap_im_feishu_resp_append(markdown,
                                                 "](feishu:file:",
                                                 sizeof("](feishu:file:") - 1)) != ESP_OK ||
                (err = cap_im_feishu_resp_append(markdown, key, strlen(key))) != ESP_OK ||
                (err = cap_im_feishu_resp_append(markdown, ")", 1)) != ESP_OK) {
            return err;
        }
        return ESP_OK;
    }

    if (strcmp(tag, "code_block") == 0) {
        language_json = cJSON_GetObjectItem(element, "language");
        if ((err = cap_im_feishu_resp_append(markdown, "\n```", 4)) != ESP_OK) {
            return err;
        }
        if (cJSON_IsString(language_json) && language_json->valuestring && language_json->valuestring[0] &&
                (err = cap_im_feishu_resp_append(markdown,
                                                 language_json->valuestring,
                                                 strlen(language_json->valuestring))) != ESP_OK) {
            return err;
        }
        if ((err = cap_im_feishu_resp_append(markdown, "\n", 1)) != ESP_OK ||
                (err = cap_im_feishu_resp_append(markdown, text, strlen(text))) != ESP_OK ||
                (err = cap_im_feishu_resp_append(markdown, "\n```\n", 5)) != ESP_OK) {
            return err;
        }
        return ESP_OK;
    }

    if (strcmp(tag, "hr") == 0) {
        return cap_im_feishu_resp_append(markdown, "\n---\n", 5);
    }

    return cap_im_feishu_resp_append(markdown, text, strlen(text));
}

static char *cap_im_feishu_extract_post_markdown(const char *content_json,
                                                 const char *chat_id,
                                                 const char *sender_id,
                                                 const char *message_id)
{
    cJSON *root = NULL;
    cJSON *body = NULL;
    cJSON *title_json = NULL;
    cJSON *content_json_array = NULL;
    cJSON *paragraph = NULL;
    cJSON *element = NULL;
    cap_im_feishu_resp_t markdown = {0};
    char *result = NULL;
    esp_err_t err;

    if (!content_json || !content_json[0]) {
        return NULL;
    }

    root = cJSON_Parse(content_json);
    if (!root) {
        return NULL;
    }

    body = cap_im_feishu_post_select_body(root);
    content_json_array = cJSON_IsObject(body) ? cJSON_GetObjectItem(body, "content") : NULL;
    if (!cJSON_IsArray(content_json_array)) {
        cJSON_Delete(root);
        return NULL;
    }

    err = cap_im_feishu_resp_init(&markdown);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return NULL;
    }

    title_json = cJSON_GetObjectItem(body, "title");
    if (cJSON_IsString(title_json) && title_json->valuestring && title_json->valuestring[0]) {
        err = cap_im_feishu_resp_append(&markdown, "**", 2);
        if (err == ESP_OK) {
            err = cap_im_feishu_resp_append(&markdown, title_json->valuestring, strlen(title_json->valuestring));
        }
        if (err == ESP_OK) {
            err = cap_im_feishu_resp_append(&markdown, "**\n\n", 4);
        }
        if (err != ESP_OK) {
            cap_im_feishu_resp_free(&markdown);
            cJSON_Delete(root);
            return NULL;
        }
    }

    cJSON_ArrayForEach(paragraph, content_json_array) {
        if (!cJSON_IsArray(paragraph)) {
            continue;
        }

        cJSON_ArrayForEach(element, paragraph) {
            err = cap_im_feishu_post_append_element(&markdown, element, chat_id, sender_id, message_id);
            if (err != ESP_OK) {
                cap_im_feishu_resp_free(&markdown);
                cJSON_Delete(root);
                return NULL;
            }
        }

        err = cap_im_feishu_resp_append(&markdown, "\n", 1);
        if (err != ESP_OK) {
            cap_im_feishu_resp_free(&markdown);
            cJSON_Delete(root);
            return NULL;
        }
    }

    while (markdown.len > 0 &&
            (markdown.buf[markdown.len - 1] == '\n' || markdown.buf[markdown.len - 1] == '\r' ||
             markdown.buf[markdown.len - 1] == ' ' || markdown.buf[markdown.len - 1] == '\t')) {
        markdown.len--;
        markdown.buf[markdown.len] = '\0';
    }

    if (markdown.len > 0) {
        result = markdown.buf;
        markdown.buf = NULL;
    }

    cap_im_feishu_resp_free(&markdown);
    cJSON_Delete(root);
    return result;
}

static void cap_im_feishu_handle_message_event(cJSON *event)
{
    cJSON *message = NULL;
    cJSON *sender = NULL;
    cJSON *sender_id_json = NULL;
    cJSON *open_id_json = NULL;
    cJSON *message_id_json = NULL;
    cJSON *chat_id_json = NULL;
    cJSON *chat_type_json = NULL;
    cJSON *message_type_json = NULL;
    cJSON *content_json = NULL;
    const char *message_id = NULL;
    const char *chat_id = NULL;
    const char *chat_type = "p2p";
    const char *message_type = "text";
    const char *sender_id = "";
    const char *route_id = NULL;

    if (!event) {
        return;
    }

    message = cJSON_GetObjectItem(event, "message");
    if (!cJSON_IsObject(message)) {
        return;
    }

    sender = cJSON_GetObjectItem(event, "sender");
    sender_id_json = cJSON_IsObject(sender) ? cJSON_GetObjectItem(sender, "sender_id") : NULL;
    open_id_json = cJSON_IsObject(sender_id_json) ? cJSON_GetObjectItem(sender_id_json, "open_id") : NULL;
    message_id_json = cJSON_GetObjectItem(message, "message_id");
    chat_id_json = cJSON_GetObjectItem(message, "chat_id");
    chat_type_json = cJSON_GetObjectItem(message, "chat_type");
    message_type_json = cJSON_GetObjectItem(message, "message_type");
    content_json = cJSON_GetObjectItem(message, "content");

    if (!cJSON_IsString(chat_id_json) || !chat_id_json->valuestring ||
            !cJSON_IsString(content_json) || !content_json->valuestring) {
        return;
    }

    message_id = cJSON_IsString(message_id_json) ? message_id_json->valuestring : "";
    chat_id = chat_id_json->valuestring;
    if (cJSON_IsString(chat_type_json) && chat_type_json->valuestring) {
        chat_type = chat_type_json->valuestring;
    }
    if (cJSON_IsString(message_type_json) && message_type_json->valuestring) {
        message_type = message_type_json->valuestring;
    }
    if (cJSON_IsString(open_id_json) && open_id_json->valuestring) {
        sender_id = open_id_json->valuestring;
    }

    if (message_id[0] && cap_im_feishu_dedup_check_and_record(message_id)) {
        ESP_LOGD(TAG, "Skip duplicate Feishu message %s", message_id);
        return;
    }

    route_id = chat_id;
    if (strcmp(chat_type, "p2p") == 0 && sender_id[0]) {
        route_id = sender_id;
    }

    if (strcmp(message_type, "text") == 0) {
        char *text = cap_im_feishu_extract_text(content_json->valuestring);
        char *clean = NULL;
        esp_err_t err;

        if (!text) {
            ESP_LOGW(TAG, "Feishu text content parse failed message=%s", message_id);
            return;
        }

        clean = text;
        if (strncmp(clean, "@_user_1 ", 9) == 0) {
            clean += 9;
        }
        while (*clean == ' ' || *clean == '\n') {
            clean++;
        }
        if (!clean[0]) {
            free(text);
            return;
        }

        err = cap_im_feishu_publish_inbound_text(route_id, sender_id, message_id, clean);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Feishu inbound publish failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Feishu text inbound chat=%s route=%s message=%s", chat_id, route_id, message_id);
        }
        free(text);
        return;
    }

    if (strcmp(message_type, "post") == 0) {
        char *markdown = cap_im_feishu_extract_post_markdown(content_json->valuestring,
                                                             route_id,
                                                             sender_id,
                                                             message_id);
        char *clean = NULL;
        esp_err_t err;

        if (!markdown) {
            ESP_LOGW(TAG, "Feishu post content parse failed message=%s", message_id);
            return;
        }

        clean = markdown;
        if (strncmp(clean, "@_user_1 ", 9) == 0) {
            clean += 9;
        }
        while (*clean == ' ' || *clean == '\n') {
            clean++;
        }
        if (!clean[0]) {
            free(markdown);
            return;
        }

        err = cap_im_feishu_publish_inbound_text(route_id, sender_id, message_id, clean);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Feishu post inbound publish failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Feishu post inbound chat=%s route=%s message=%s", chat_id, route_id, message_id);
        }
        free(markdown);
        return;
    }

    if (strcmp(message_type, "image") == 0 || strcmp(message_type, "file") == 0) {
        cap_im_feishu_queue_attachment(route_id,
                                       sender_id,
                                       message_id,
                                       message_type,
                                       content_json->valuestring);
        return;
    }

    ESP_LOGI(TAG, "Ignore Feishu message_type=%s message=%s", message_type, message_id);
}

static void cap_im_feishu_process_ws_event_json(const char *json, size_t len)
{
    cJSON *root = NULL;
    cJSON *event = NULL;
    cJSON *header = NULL;
    cJSON *event_type = NULL;

    if (!json || len == 0) {
        return;
    }

    root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "Feishu WS JSON parse failed");
        return;
    }

    event = cJSON_GetObjectItem(root, "event");
    header = cJSON_GetObjectItem(root, "header");
    event_type = cJSON_IsObject(header) ? cJSON_GetObjectItem(header, "event_type") : NULL;
    if (cJSON_IsObject(event) &&
            (!event_type || (cJSON_IsString(event_type) &&
                             strcmp(event_type->valuestring, "im.message.receive_v1") == 0))) {
        cap_im_feishu_handle_message_event(event);
    }

    cJSON_Delete(root);
}

static void cap_im_feishu_handle_ws_frame(const uint8_t *buf, size_t len)
{
    cap_im_feishu_ws_frame_t frame = {0};
    const char *type = NULL;

    if (!cap_im_feishu_pb_parse_frame(buf, len, &frame)) {
        ESP_LOGW(TAG, "Feishu WS frame parse failed");
        return;
    }

    type = cap_im_feishu_ws_header_value(&frame, "type");
    if (frame.method == 0) {
        if (type && strcmp(type, "pong") == 0 && frame.payload && frame.payload_len > 0) {
            cJSON *cfg = cJSON_ParseWithLength((const char *)frame.payload, frame.payload_len);

            if (cfg) {
                cJSON *ping_json = cJSON_GetObjectItem(cfg, "PingInterval");

                if (cJSON_IsNumber(ping_json)) {
                    s_feishu.ws_ping_interval_ms = ping_json->valueint * 1000;
                }
                cJSON_Delete(cfg);
            }
        }
        return;
    }

    if (!type || strcmp(type, "event") != 0 || !frame.payload || frame.payload_len == 0) {
        return;
    }

    cap_im_feishu_process_ws_event_json((const char *)frame.payload, frame.payload_len);

    {
        const char ack_payload[] = "{\"code\":200}";
        cap_im_feishu_ws_frame_t ack = frame;

        cap_im_feishu_ws_send_frame(&ack, (const uint8_t *)ack_payload, strlen(ack_payload), 1000);
    }
}

static void cap_im_feishu_ws_event_handler(void *arg,
                                           esp_event_base_t base,
                                           int32_t event_id,
                                           void *event_data)
{
    static uint8_t *rx_buf = NULL;
    static size_t rx_cap = 0;
    esp_websocket_event_data_t *event = (esp_websocket_event_data_t *)event_data;

    (void)arg;
    (void)base;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_feishu.ws_connected = true;
        s_feishu.ws_ever_connected = true;
        s_feishu.ws_disconnect_since_ms = 0;
        ESP_LOGI(TAG, "Feishu WS connected");
        return;
    }

    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_feishu.ws_connected = false;
        if (s_feishu.ws_ever_connected && s_feishu.ws_disconnect_since_ms == 0) {
            s_feishu.ws_disconnect_since_ms = cap_im_feishu_now_ms();
        }
        ESP_LOGW(TAG, "Feishu WS disconnected");
        return;
    }

    if (event_id != WEBSOCKET_EVENT_DATA || !event || event->op_code != WS_TRANSPORT_OPCODES_BINARY) {
        return;
    }

    {
        size_t need = event->payload_offset + event->data_len;

        if (event->payload_offset == 0) {
            free(rx_buf);
            rx_cap = event->payload_len > need ? event->payload_len : need;
            rx_buf = malloc(rx_cap);
            if (!rx_buf) {
                rx_cap = 0;
                return;
            }
        } else if (!rx_buf || need > rx_cap) {
            return;
        }

        memcpy(rx_buf + event->payload_offset, event->data_ptr, event->data_len);
        if (need >= event->payload_len) {
            cap_im_feishu_handle_ws_frame(rx_buf, event->payload_len);
            free(rx_buf);
            rx_buf = NULL;
            rx_cap = 0;
        }
    }
}

static void cap_im_feishu_ws_task(void *arg)
{
    (void)arg;

    while (!s_feishu.stop_requested) {
        int64_t connect_started_ms = 0;
        int64_t last_ping_ms = 0;

        if (cap_im_feishu_pull_ws_config() != ESP_OK) {
            if (s_feishu.stop_requested) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(CAP_IM_FEISHU_RECONNECT_DELAY_MS));
            continue;
        }

        {
            esp_websocket_client_config_t ws_config = {
                .uri = s_feishu.ws_url,
                .buffer_size = 2048,
                .task_stack = 16 * 1024,
                .reconnect_timeout_ms = s_feishu.ws_reconnect_interval_ms,
                .network_timeout_ms = 10000,
                .disable_auto_reconnect = false,
                .crt_bundle_attach = esp_crt_bundle_attach,
            };

            s_feishu.ws_client = esp_websocket_client_init(&ws_config);
        }
        if (!s_feishu.ws_client) {
            if (s_feishu.stop_requested) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(CAP_IM_FEISHU_RECONNECT_DELAY_MS));
            continue;
        }

        s_feishu.ws_connected = false;
        s_feishu.ws_ever_connected = false;
        s_feishu.ws_disconnect_since_ms = 0;
        esp_websocket_register_events(s_feishu.ws_client,
                                      WEBSOCKET_EVENT_ANY,
                                      cap_im_feishu_ws_event_handler,
                                      NULL);
        if (esp_websocket_client_start(s_feishu.ws_client) != ESP_OK) {
            ESP_LOGE(TAG, "Feishu WS start failed");
            esp_websocket_client_destroy(s_feishu.ws_client);
            s_feishu.ws_client = NULL;
            if (s_feishu.stop_requested) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(CAP_IM_FEISHU_RECONNECT_DELAY_MS));
            continue;
        }

        connect_started_ms = cap_im_feishu_now_ms();
        while (s_feishu.ws_client && !s_feishu.stop_requested) {
            int64_t now_ms = cap_im_feishu_now_ms();

            if (s_feishu.ws_connected && now_ms - last_ping_ms >= s_feishu.ws_ping_interval_ms) {
                cap_im_feishu_ws_frame_t ping = {0};

                ping.service = s_feishu.ws_service_id;
                ping.header_count = 1;
                strlcpy(ping.headers[0].key, "type", sizeof(ping.headers[0].key));
                strlcpy(ping.headers[0].value, "ping", sizeof(ping.headers[0].value));
                cap_im_feishu_ws_send_frame(&ping, NULL, 0, 1000);
                last_ping_ms = now_ms;
            }

            if (!s_feishu.ws_ever_connected) {
                if (!esp_websocket_client_is_connected(s_feishu.ws_client) &&
                        now_ms - connect_started_ms >= CAP_IM_FEISHU_INITIAL_CONNECT_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "Feishu WS initial connect timeout");
                    break;
                }
            } else if (!esp_websocket_client_is_connected(s_feishu.ws_client) && !s_feishu.ws_connected) {
                if (s_feishu.ws_disconnect_since_ms == 0) {
                    s_feishu.ws_disconnect_since_ms = now_ms;
                }
                if (now_ms - s_feishu.ws_disconnect_since_ms >=
                        s_feishu.ws_reconnect_interval_ms + s_feishu.ws_reconnect_nonce_ms + 5000) {
                    ESP_LOGW(TAG, "Feishu WS reconnect grace expired");
                    break;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (s_feishu.ws_client) {
            esp_websocket_client_stop(s_feishu.ws_client);
            esp_websocket_client_destroy(s_feishu.ws_client);
            s_feishu.ws_client = NULL;
        }
        s_feishu.ws_connected = false;
        if (!s_feishu.stop_requested) {
            vTaskDelay(pdMS_TO_TICKS(CAP_IM_FEISHU_RECONNECT_DELAY_MS));
        }
    }

    s_feishu.ws_client = NULL;
    s_feishu.ws_task = NULL;
    claw_task_delete(NULL);
}

static esp_err_t cap_im_feishu_gateway_init(void)
{
    if (!s_feishu.app_id[0] || !s_feishu.app_secret[0]) {
        ESP_LOGW(TAG, "Feishu credentials not configured");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Feishu configured (app_id=%.8s...)", s_feishu.app_id);
    return ESP_OK;
}

static esp_err_t cap_im_feishu_gateway_start(void)
{
    return cap_im_feishu_start();
}

static esp_err_t cap_im_feishu_gateway_stop(void)
{
    return cap_im_feishu_stop();
}

static esp_err_t cap_im_feishu_send_message_content(const char *chat_id,
                                                    const char *msg_type,
                                                    const char *content_json)
{
    const char *id_type = NULL;
    cJSON *body = NULL;
    char *body_str = NULL;
    char *response = NULL;
    char url[CAP_IM_FEISHU_URL_LEN];
    esp_err_t err;

    if (!chat_id || !chat_id[0] || !msg_type || !msg_type[0] || !content_json || !content_json[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_feishu.app_id[0] || !s_feishu.app_secret[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    id_type = strncmp(chat_id, "ou_", 3) == 0 ? "open_id" : "chat_id";
    snprintf(url, sizeof(url), "%s?receive_id_type=%s", CAP_IM_FEISHU_SEND_MSG_URL, id_type);

    body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "receive_id", chat_id);
    cJSON_AddStringToObject(body, "msg_type", msg_type);
    cJSON_AddStringToObject(body, "content", content_json);
    body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_feishu_api_call(url, "POST", body_str, &response);
    free(body_str);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_im_feishu_validate_api_success_response(response);
    free(response);
    return err;
}

static esp_err_t cap_im_feishu_build_markdown_card_content(const char *message,
                                                           char **out_content)
{
    cJSON *card = NULL;
    cJSON *body = NULL;
    cJSON *elements = NULL;
    cJSON *element = NULL;
    char *content_str = NULL;
    size_t message_len = 0;
    size_t content_len = 0;
    esp_err_t err = ESP_OK;

    if (out_content) {
        *out_content = NULL;
    }
    if (!message || !message[0] || !out_content) {
        return ESP_ERR_INVALID_ARG;
    }

    message_len = strlen(message);
    if (message_len > CAP_IM_FEISHU_MAX_CARD_MARKDOWN_LEN) {
        ESP_LOGW(TAG,
                 "Feishu markdown card skipped, message too large len=%u max=%u",
                 (unsigned int)message_len,
                 (unsigned int)CAP_IM_FEISHU_MAX_CARD_MARKDOWN_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    card = cJSON_CreateObject();
    if (!card) {
        return ESP_ERR_NO_MEM;
    }

    body = cJSON_CreateObject();
    elements = cJSON_CreateArray();
    element = cJSON_CreateObject();
    if (!body || !elements || !element ||
            !cJSON_AddStringToObject(card, "schema", "2.0") ||
            !cJSON_AddStringToObject(element, "tag", "markdown") ||
            !cJSON_AddStringToObject(element, "content", message)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    if (!cJSON_AddItemToArray(elements, element)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    element = NULL;

    if (!cJSON_AddItemToObject(body, "elements", elements)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    elements = NULL;

    if (!cJSON_AddItemToObject(card, "body", body)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    body = NULL;

    content_str = cJSON_PrintUnformatted(card);
    if (!content_str) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    content_len = strlen(content_str);
    if (content_len > CAP_IM_FEISHU_MAX_CARD_CONTENT_LEN) {
        ESP_LOGW(TAG,
                 "Feishu markdown card skipped, payload too large len=%u max=%u",
                 (unsigned int)content_len,
                 (unsigned int)CAP_IM_FEISHU_MAX_CARD_CONTENT_LEN);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    *out_content = content_str;
    content_str = NULL;

cleanup:
    free(content_str);
    cJSON_Delete(element);
    cJSON_Delete(elements);
    cJSON_Delete(body);
    cJSON_Delete(card);
    return err;
}

static esp_err_t cap_im_feishu_send_markdown_card(const char *chat_id,
                                                  const char *message)
{
    char *card_content = NULL;
    esp_err_t err;

    err = cap_im_feishu_build_markdown_card_content(message, &card_content);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_im_feishu_send_message_content(chat_id, "interactive", card_content);
    free(card_content);
    return err;
}

static esp_err_t cap_im_feishu_upload_media(const char *path,
                                            bool is_image,
                                            char *out_key,
                                            size_t out_key_size)
{
    struct stat st = {0};
    FILE *file = NULL;
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    cap_im_feishu_resp_t resp = {0};
    const char *file_name = NULL;
    const char *mime = NULL;
    const char *url = NULL;
    const char *field_name = NULL;
    const char *key_name = NULL;
    char auth_header[CAP_IM_FEISHU_TOKEN_LEN + 16];
    char content_type[128];
    char preamble1[256];
    char preamble2[384];
    char part_file[384];
    char closing[64];
    int preamble1_len = 0;
    int preamble2_len = 0;
    int part_file_len = 0;
    int closing_len = 0;
    size_t content_length = 0;
    int status = 0;
    esp_err_t err = ESP_FAIL;

    if (out_key && out_key_size > 0) {
        out_key[0] = '\0';
    }
    if (!path || !path[0] || !out_key || out_key_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_feishu.app_id[0] || !s_feishu.app_secret[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stat(path, &st) != 0) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (!S_ISREG(st.st_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (st.st_size <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = cap_im_feishu_get_tenant_token();
    if (err != ESP_OK) {
        return err;
    }

    file_name = cap_im_feishu_basename(path);
    mime = cap_im_feishu_guess_upload_mime(path, is_image);
    url = is_image ? CAP_IM_FEISHU_UPLOAD_IMAGE_URL : CAP_IM_FEISHU_UPLOAD_FILE_URL;
    field_name = is_image ? "image" : "file";
    key_name = is_image ? "image_key" : "file_key";
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_feishu.tenant_token);

    if (is_image) {
        preamble1_len = snprintf(preamble1,
                                 sizeof(preamble1),
                                 "--" CAP_IM_FEISHU_MULTIPART_BOUNDARY "\r\n"
                                 "Content-Disposition: form-data; name=\"image_type\"\r\n\r\n"
                                 "message\r\n");
    } else {
        preamble1_len = snprintf(preamble1,
                                 sizeof(preamble1),
                                 "--" CAP_IM_FEISHU_MULTIPART_BOUNDARY "\r\n"
                                 "Content-Disposition: form-data; name=\"file_type\"\r\n\r\n"
                                 "stream\r\n");
        preamble2_len = snprintf(preamble2,
                                 sizeof(preamble2),
                                 "--" CAP_IM_FEISHU_MULTIPART_BOUNDARY "\r\n"
                                 "Content-Disposition: form-data; name=\"file_name\"\r\n\r\n"
                                 "%s\r\n",
                                 file_name);
    }
    part_file_len = snprintf(part_file,
                             sizeof(part_file),
                             "--" CAP_IM_FEISHU_MULTIPART_BOUNDARY "\r\n"
                             "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                             "Content-Type: %s\r\n\r\n",
                             field_name,
                             file_name,
                             mime);
    closing_len = snprintf(closing, sizeof(closing),
                           "\r\n--" CAP_IM_FEISHU_MULTIPART_BOUNDARY "--\r\n");
    if (preamble1_len <= 0 || part_file_len <= 0 || closing_len <= 0 ||
            preamble1_len >= (int)sizeof(preamble1) ||
            preamble2_len >= (int)sizeof(preamble2) ||
            part_file_len >= (int)sizeof(part_file) ||
            closing_len >= (int)sizeof(closing)) {
        return ESP_ERR_INVALID_SIZE;
    }

    content_length = (size_t)preamble1_len + (size_t)preamble2_len + (size_t)part_file_len +
                     (size_t)st.st_size + (size_t)closing_len;

    file = fopen(path, "rb");
    if (!file) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    err = cap_im_feishu_resp_init(&resp);
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }

    config.url = url;
    config.timeout_ms = 30000;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    client = esp_http_client_init(&config);
    if (!client) {
        fclose(file);
        cap_im_feishu_resp_free(&resp);
        return ESP_FAIL;
    }

    snprintf(content_type,
             sizeof(content_type),
             "multipart/form-data; boundary=" CAP_IM_FEISHU_MULTIPART_BOUNDARY);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", content_type);

    err = esp_http_client_open(client, (int)content_length);
    if (err == ESP_OK) {
        err = cap_im_feishu_http_client_write_all(client, preamble1, (size_t)preamble1_len);
    }
    if (err == ESP_OK && preamble2_len > 0) {
        err = cap_im_feishu_http_client_write_all(client, preamble2, (size_t)preamble2_len);
    }
    if (err == ESP_OK) {
        err = cap_im_feishu_http_client_write_all(client, part_file, (size_t)part_file_len);
    }
    if (err == ESP_OK) {
        err = cap_im_feishu_stream_file_to_http_client(client, file);
    }
    if (err == ESP_OK) {
        err = cap_im_feishu_http_client_write_all(client, closing, (size_t)closing_len);
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        err = ESP_FAIL;
    }

    status = esp_http_client_get_status_code(client);
    if (err == ESP_OK) {
        while (1) {
            char buf[512];
            int read_len = esp_http_client_read(client, buf, sizeof(buf));

            if (read_len < 0) {
                err = ESP_FAIL;
                break;
            }
            if (read_len == 0) {
                break;
            }
            err = cap_im_feishu_resp_append(&resp, buf, (size_t)read_len);
            if (err != ESP_OK) {
                break;
            }
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    fclose(file);

    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Feishu %s upload failed path=%s err=%s",
                 is_image ? "image" : "file",
                 path,
                 esp_err_to_name(err));
        cap_im_feishu_resp_free(&resp);
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG,
                 "Feishu %s upload HTTP %d path=%s body=%s",
                 is_image ? "image" : "file",
                 status,
                 path,
                 resp.buf ? resp.buf : "");
        cap_im_feishu_resp_free(&resp);
        return ESP_FAIL;
    }

    {
        cJSON *root = cJSON_Parse(resp.buf);
        cJSON *data_json = NULL;
        cJSON *key_json = NULL;

        if (!root) {
            cap_im_feishu_resp_free(&resp);
            return ESP_FAIL;
        }
        if (!cJSON_IsNumber(cJSON_GetObjectItem(root, "code")) ||
                cJSON_GetObjectItem(root, "code")->valueint != 0) {
            ESP_LOGW(TAG,
                     "Feishu %s upload response invalid path=%s body=%s",
                     is_image ? "image" : "file",
                     path,
                     resp.buf ? resp.buf : "");
            cJSON_Delete(root);
            cap_im_feishu_resp_free(&resp);
            return ESP_FAIL;
        }

        data_json = cJSON_GetObjectItem(root, "data");
        key_json = cJSON_IsObject(data_json) ? cJSON_GetObjectItem(data_json, key_name) :
                                               cJSON_GetObjectItem(root, key_name);
        if (!cJSON_IsString(key_json) || !key_json->valuestring || !key_json->valuestring[0]) {
            cJSON_Delete(root);
            cap_im_feishu_resp_free(&resp);
            return ESP_ERR_NOT_FOUND;
        }

        strlcpy(out_key, key_json->valuestring, out_key_size);
        cJSON_Delete(root);
    }

    cap_im_feishu_resp_free(&resp);
    return ESP_OK;
}

static esp_err_t cap_im_feishu_send_media_internal(const char *chat_id,
                                                   const char *path,
                                                   const char *caption,
                                                   bool is_image)
{
    cJSON *content = NULL;
    char *content_str = NULL;
    char media_key[CAP_IM_FEISHU_MEDIA_KEY_LEN];
    esp_err_t err;

    err = cap_im_feishu_upload_media(path, is_image, media_key, sizeof(media_key));
    if (err != ESP_OK) {
        return err;
    }

    content = cJSON_CreateObject();
    if (!content) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(content, is_image ? "image_key" : "file_key", media_key);
    content_str = cJSON_PrintUnformatted(content);
    cJSON_Delete(content);
    if (!content_str) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_feishu_send_message_content(chat_id,
                                             is_image ? "image" : "file",
                                             content_str);
    free(content_str);
    if (err != ESP_OK) {
        return err;
    }

    if (caption && caption[0]) {
        err = cap_im_feishu_send_text(chat_id, caption);
        if (err != ESP_OK) {
            return err;
        }
    }

    ESP_LOGI(TAG,
             "Feishu %s send success to %s: %s",
             is_image ? "image" : "file",
             chat_id,
             path);
    return ESP_OK;
}

static esp_err_t cap_im_feishu_send_message_execute(const char *input_json,
                                                    const claw_cap_call_context_t *ctx,
                                                    char *output,
                                                    size_t output_size)
{
    cJSON *root = NULL;
    cJSON *chat_id_json = NULL;
    cJSON *message_json = NULL;
    const char *chat_id = NULL;
    const char *message = NULL;
    esp_err_t err;

    (void)ctx;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid json\"}");
        return ESP_ERR_INVALID_ARG;
    }

    chat_id_json = cJSON_GetObjectItem(root, "chat_id");
    message_json = cJSON_GetObjectItem(root, "message");
    if (cJSON_IsString(chat_id_json) && chat_id_json->valuestring && chat_id_json->valuestring[0]) {
        chat_id = chat_id_json->valuestring;
    } else if (ctx && ctx->chat_id && ctx->chat_id[0]) {
        chat_id = ctx->chat_id;
    }
    message = cJSON_IsString(message_json) ? message_json->valuestring : NULL;
    if (!chat_id || !chat_id[0] || !message || !message[0]) {
        cJSON_Delete(root);
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"chat_id and message are required (chat_id may come from ctx)\"}");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_feishu_send_markdown_card(chat_id, message);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Feishu markdown card send failed chat=%s err=%s, falling back to text",
                 chat_id,
                 esp_err_to_name(err));
        err = cap_im_feishu_send_text(chat_id, message);
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t cap_im_feishu_send_media_execute(const char *input_json,
                                                  const claw_cap_call_context_t *ctx,
                                                  char *output,
                                                  size_t output_size,
                                                  bool is_image)
{
    cJSON *root = NULL;
    cJSON *chat_id_json = NULL;
    cJSON *path_json = NULL;
    cJSON *caption_json = NULL;
    const char *chat_id = NULL;
    const char *path = NULL;
    const char *caption = NULL;
    esp_err_t err;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid json\"}");
        return ESP_ERR_INVALID_ARG;
    }

    chat_id_json = cJSON_GetObjectItem(root, "chat_id");
    path_json = cJSON_GetObjectItem(root, "path");
    caption_json = cJSON_GetObjectItem(root, "caption");
    if (cJSON_IsString(chat_id_json) && chat_id_json->valuestring && chat_id_json->valuestring[0]) {
        chat_id = chat_id_json->valuestring;
    } else if (ctx && ctx->chat_id && ctx->chat_id[0]) {
        chat_id = ctx->chat_id;
    }
    if (cJSON_IsString(path_json) && path_json->valuestring && path_json->valuestring[0]) {
        path = path_json->valuestring;
    }
    if (cJSON_IsString(caption_json) && caption_json->valuestring) {
        caption = caption_json->valuestring;
    }

    if (!chat_id || !path) {
        cJSON_Delete(root);
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"chat_id and path are required (chat_id may come from ctx)\"}");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_feishu_send_media_internal(chat_id, path, caption, is_image);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t cap_im_feishu_send_image_execute(const char *input_json,
                                                  const claw_cap_call_context_t *ctx,
                                                  char *output,
                                                  size_t output_size)
{
    return cap_im_feishu_send_media_execute(input_json, ctx, output, output_size, true);
}

static esp_err_t cap_im_feishu_send_file_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size)
{
    return cap_im_feishu_send_media_execute(input_json, ctx, output, output_size, false);
}

static const claw_cap_descriptor_t s_feishu_descriptors[] = {
    {
        .id = "feishu_gateway",
        .name = "feishu_gateway",
        .family = "im",
        .description = "Feishu WebSocket gateway event source.",
        .kind = CLAW_CAP_KIND_EVENT_SOURCE,
        .cap_flags = CLAW_CAP_FLAG_EMITS_EVENTS | CLAW_CAP_FLAG_SUPPORTS_LIFECYCLE,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .init = cap_im_feishu_gateway_init,
        .start = cap_im_feishu_gateway_start,
        .stop = cap_im_feishu_gateway_stop,
    },
    {
        .id = "feishu_send_message",
        .name = "feishu_send_message",
        .family = "im",
        .description = "Send a text message to a Feishu chat_id or user open_id.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"}},\"required\":[\"chat_id\",\"message\"]}",
        .execute = cap_im_feishu_send_message_execute,
    },
    {
        .id = "feishu_send_image",
        .name = "feishu_send_image",
        .family = "im",
        .description = "Send an image file from a local path to a Feishu chat.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"caption\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        .execute = cap_im_feishu_send_image_execute,
    },
    {
        .id = "feishu_send_file",
        .name = "feishu_send_file",
        .family = "im",
        .description = "Send a file from a local path to a Feishu chat.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"caption\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        .execute = cap_im_feishu_send_file_execute,
    },
};

static const claw_cap_group_t s_feishu_group = {
    .group_id = "cap_im_feishu",
    .descriptors = s_feishu_descriptors,
    .descriptor_count = sizeof(s_feishu_descriptors) / sizeof(s_feishu_descriptors[0]),
};

esp_err_t cap_im_feishu_register_group(void)
{
    if (claw_cap_group_exists(s_feishu_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_feishu_group);
}

esp_err_t cap_im_feishu_set_credentials(const char *app_id, const char *app_secret)
{
    if (!app_id || !app_secret) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_feishu.app_id, app_id, sizeof(s_feishu.app_id));
    strlcpy(s_feishu.app_secret, app_secret, sizeof(s_feishu.app_secret));
    s_feishu.tenant_token[0] = '\0';
    s_feishu.token_expire_time_ms = 0;
    return ESP_OK;
}

esp_err_t cap_im_feishu_set_attachment_config(const cap_im_feishu_attachment_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    s_feishu.enable_inbound_attachments = config->enable_inbound_attachments;
    s_feishu.max_inbound_file_bytes = config->max_inbound_file_bytes;
    if (config->storage_root_dir) {
        strlcpy(s_feishu.attachment_root_dir,
                config->storage_root_dir,
                sizeof(s_feishu.attachment_root_dir));
    } else {
        s_feishu.attachment_root_dir[0] = '\0';
    }
    return ESP_OK;
}

esp_err_t cap_im_feishu_start(void)
{
    BaseType_t ok;

    if (!s_feishu.app_id[0] || !s_feishu.app_secret[0]) {
        ESP_LOGW(TAG, "Feishu not configured, skip start");
        return ESP_OK;
    }
    if (s_feishu.ws_task) {
        return ESP_OK;
    }
    if (!s_feishu.attachment_queue) {
        s_feishu.attachment_queue = xQueueCreate(CAP_IM_FEISHU_ATTACHMENT_QUEUE_LEN,
                                                 sizeof(cap_im_feishu_attachment_job_t *));
        if (!s_feishu.attachment_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_feishu.attachment_task) {
        ok = claw_task_create(&(claw_task_config_t){
                                  .name = "feishu_attach",
                                  .stack_size = 8192,
                                  .priority = 5,
                                  .core_id = tskNO_AFFINITY,
                                  .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                              },
                              cap_im_feishu_attachment_task,
                              NULL,
                              &s_feishu.attachment_task);
        if (ok != pdPASS) {
            vQueueDelete(s_feishu.attachment_queue);
            s_feishu.attachment_queue = NULL;
            s_feishu.attachment_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    s_feishu.stop_requested = false;
    ok = claw_task_create(&(claw_task_config_t){
                              .name = "feishu_ws",
                              .stack_size = 8192,
                              .priority = 5,
                              .core_id = tskNO_AFFINITY,
                              .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                          },
                          cap_im_feishu_ws_task,
                          NULL,
                          &s_feishu.ws_task);
    if (ok != pdPASS) {
        if (s_feishu.attachment_task) {
            claw_task_delete(s_feishu.attachment_task);
            s_feishu.attachment_task = NULL;
        }
        if (s_feishu.attachment_queue) {
            vQueueDelete(s_feishu.attachment_queue);
            s_feishu.attachment_queue = NULL;
        }
        s_feishu.ws_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t cap_im_feishu_stop(void)
{
    TickType_t deadline = 0;

    if (!s_feishu.ws_task && !s_feishu.attachment_task) {
        return ESP_OK;
    }

    s_feishu.stop_requested = true;
    if (s_feishu.ws_client) {
        esp_websocket_client_stop(s_feishu.ws_client);
    }

    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    while ((s_feishu.ws_task || s_feishu.attachment_task) && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_feishu.ws_task || s_feishu.attachment_task) {
        ESP_LOGW(TAG, "Feishu stop timed out");
        return ESP_ERR_TIMEOUT;
    }
    if (s_feishu.attachment_queue) {
        cap_im_feishu_attachment_job_t *job = NULL;

        while (xQueueReceive(s_feishu.attachment_queue, &job, 0) == pdTRUE) {
            cap_im_feishu_free_attachment_job(job);
        }
        vQueueDelete(s_feishu.attachment_queue);
        s_feishu.attachment_queue = NULL;
    }

    s_feishu.ws_client = NULL;
    s_feishu.ws_connected = false;
    return ESP_OK;
}

esp_err_t cap_im_feishu_send_text(const char *chat_id, const char *text)
{
    size_t offset = 0;
    size_t text_len = 0;

    if (!chat_id || !chat_id[0] || !text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_feishu.app_id[0] || !s_feishu.app_secret[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    text_len = strlen(text);

    while (offset < text_len) {
        size_t chunk_len = text_len - offset;
        cJSON *content = NULL;
        char *content_str = NULL;
        char *segment = NULL;
        esp_err_t err;

        if (chunk_len > CAP_IM_FEISHU_MAX_CHUNK_LEN) {
            chunk_len = CAP_IM_FEISHU_MAX_CHUNK_LEN;
        }

        segment = calloc(1, chunk_len + 1);
        if (!segment) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(segment, text + offset, chunk_len);
        segment[chunk_len] = '\0';

        content = cJSON_CreateObject();
        if (!content) {
            free(segment);
            cJSON_Delete(content);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(content, "text", segment);
        content_str = cJSON_PrintUnformatted(content);
        cJSON_Delete(content);
        free(segment);
        if (!content_str) {
            return ESP_ERR_NO_MEM;
        }

        err = cap_im_feishu_send_message_content(chat_id, "text", content_str);
        free(content_str);
        if (err != ESP_OK) {
            return err;
        }
        offset += chunk_len;
    }

    return ESP_OK;
}

esp_err_t cap_im_feishu_send_image(const char *chat_id, const char *path, const char *caption)
{
    if (!chat_id || !chat_id[0] || !path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    return cap_im_feishu_send_media_internal(chat_id, path, caption, true);
}

esp_err_t cap_im_feishu_send_file(const char *chat_id, const char *path, const char *caption)
{
    if (!chat_id || !chat_id[0] || !path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    return cap_im_feishu_send_media_internal(chat_id, path, caption, false);
}
