/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_qq.h"
#include "cap_im_attachment.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cJSON.h"
#include "claw_task.h"
#include "claw_event_publisher.h"
#include "esp_crt_bundle.h"
#include "esp_attr.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "cap_im_qq";

#define CAP_IM_QQ_TOKEN_URL            "https://bots.qq.com/app/getAppAccessToken"
#define CAP_IM_QQ_API_BASE             "https://api.sgroup.qq.com"
#define CAP_IM_QQ_GATEWAY_URL          CAP_IM_QQ_API_BASE "/gateway"
#define CAP_IM_QQ_WS_CONNECT_GRACE_MS  10000
#define CAP_IM_QQ_RECONNECT_DELAY_MS   5000
#define CAP_IM_QQ_MAX_MSG_LEN          1500
#define CAP_IM_QQ_HTTP_RESP_INIT       2048
#define CAP_IM_QQ_WS_TASK_STACK        6144
#define CAP_IM_QQ_WS_CLIENT_STACK      8192
#define CAP_IM_QQ_WS_PRIO              5
#define CAP_IM_QQ_INBOUND_QUEUE_LEN    8
#define CAP_IM_QQ_DEDUP_CACHE_SIZE     64
#define CAP_IM_QQ_PATH_BUF_SIZE        256
#define CAP_IM_QQ_NAME_BUF_SIZE        96

#define CAP_IM_QQ_WS_OP_DISPATCH        0
#define CAP_IM_QQ_WS_OP_HEARTBEAT       1
#define CAP_IM_QQ_WS_OP_IDENTIFY        2
#define CAP_IM_QQ_WS_OP_RECONNECT       7
#define CAP_IM_QQ_WS_OP_INVALID_SESSION 9
#define CAP_IM_QQ_WS_OP_HELLO           10
#define CAP_IM_QQ_WS_OP_HEARTBEAT_ACK   11

#define CAP_IM_QQ_INTENTS ((1 << 30) | (1 << 25))
#define CAP_IM_QQ_FILE_TYPE_IMAGE 1
#define CAP_IM_QQ_FILE_TYPE_FILE  4

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} cap_im_qq_http_resp_t;

typedef struct {
    FILE *file;
    size_t bytes_written;
    size_t max_bytes;
    bool limit_hit;
} cap_im_qq_download_t;

typedef struct {
    char *frame;
    size_t len;
} cap_im_qq_inbound_frame_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    int payload_len;
} cap_im_qq_ws_assembly_t;

typedef struct {
    char app_id[64];
    char app_secret[128];
    char access_token[512];
    char attachment_root_dir[128];
    size_t max_inbound_file_bytes;
    bool enable_inbound_attachments;
    int64_t token_expire_time;
    char ws_url[384];
    esp_websocket_client_handle_t ws_client;
    TaskHandle_t ws_task;
    TaskHandle_t inbound_task;
    QueueHandle_t inbound_queue;
    volatile int heartbeat_interval_ms;
    volatile int last_seq;
    volatile bool ws_connected;
    volatile bool ws_identify_pending;
    volatile bool ws_should_reconnect;
    volatile bool stop_requested;
    int msg_type;
    cap_im_qq_ws_assembly_t ws_assembly;
    uint64_t seen_msg_keys[CAP_IM_QQ_DEDUP_CACHE_SIZE];
    size_t seen_msg_idx;
} cap_im_qq_state_t;

static EXT_RAM_BSS_ATTR cap_im_qq_state_t s_qq;
static bool s_qq_initialized;

static void cap_im_qq_init_defaults(void)
{
    if (s_qq_initialized) {
        return;
    }
    s_qq.max_inbound_file_bytes = 2 * 1024 * 1024;
    s_qq.heartbeat_interval_ms = 30000;
    s_qq.last_seq = -1;
    s_qq.msg_type = 0;
    s_qq_initialized = true;
}

static int64_t cap_im_qq_now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static uint64_t cap_im_qq_fnv1a64(const char *text)
{
    uint64_t hash = 1469598103934665603ULL;

    if (!text) {
        return hash;
    }

    while (*text) {
        hash ^= (unsigned char)(*text++);
        hash *= 1099511628211ULL;
    }

    return hash;
}

static bool cap_im_qq_dedup_check_and_record(const char *message_id)
{
    uint64_t key;
    size_t i;

    if (!message_id || !message_id[0]) {
        return false;
    }

    key = cap_im_qq_fnv1a64(message_id);
    for (i = 0; i < CAP_IM_QQ_DEDUP_CACHE_SIZE; i++) {
        if (s_qq.seen_msg_keys[i] == key) {
            return true;
        }
    }

    s_qq.seen_msg_keys[s_qq.seen_msg_idx] = key;
    s_qq.seen_msg_idx = (s_qq.seen_msg_idx + 1) % CAP_IM_QQ_DEDUP_CACHE_SIZE;
    return false;
}

static const char *cap_im_qq_basename(const char *path)
{
    const char *slash = NULL;

    if (!path || !path[0]) {
        return "";
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool cap_im_qq_is_image_mime(const char *mime)
{
    return mime && strncmp(mime, "image/", 6) == 0;
}

static bool cap_im_qq_is_audio_mime(const char *mime)
{
    return mime && strncmp(mime, "audio/", 6) == 0;
}

static void cap_im_qq_reset_ws_assembly(void)
{
    free(s_qq.ws_assembly.buf);
    s_qq.ws_assembly.buf = NULL;
    s_qq.ws_assembly.len = 0;
    s_qq.ws_assembly.cap = 0;
    s_qq.ws_assembly.payload_len = 0;
}

static esp_err_t cap_im_qq_queue_inbound_frame(const char *frame, size_t frame_len)
{
    cap_im_qq_inbound_frame_t item = {0};

    if (!frame || frame_len == 0 || !s_qq.inbound_queue) {
        return ESP_ERR_INVALID_ARG;
    }

    item.frame = calloc(1, frame_len + 1);
    if (!item.frame) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(item.frame, frame, frame_len);
    item.len = frame_len;
    if (xQueueSend(s_qq.inbound_queue, &item, 0) != pdTRUE) {
        free(item.frame);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t cap_im_qq_handle_inbound_ws_data(const esp_websocket_event_data_t *event)
{
    size_t needed = 0;

    if (!event || event->op_code != 0x01 || !event->data_ptr || event->data_len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_qq.inbound_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (event->payload_len <= 0 ||
            (event->payload_offset == 0 && event->payload_len == event->data_len)) {
        return cap_im_qq_queue_inbound_frame(event->data_ptr, (size_t)event->data_len);
    }

    if (event->payload_offset == 0) {
        needed = (size_t)event->payload_len + 1;
        if (needed > s_qq.ws_assembly.cap) {
            char *tmp = realloc(s_qq.ws_assembly.buf, needed);

            if (!tmp) {
                cap_im_qq_reset_ws_assembly();
                return ESP_ERR_NO_MEM;
            }
            s_qq.ws_assembly.buf = tmp;
            s_qq.ws_assembly.cap = needed;
        }
        s_qq.ws_assembly.len = 0;
        s_qq.ws_assembly.payload_len = event->payload_len;
    } else if (!s_qq.ws_assembly.buf ||
               s_qq.ws_assembly.payload_len != event->payload_len ||
               s_qq.ws_assembly.len != (size_t)event->payload_offset) {
        cap_im_qq_reset_ws_assembly();
        return ESP_FAIL;
    }

    memcpy(s_qq.ws_assembly.buf + s_qq.ws_assembly.len, event->data_ptr, (size_t)event->data_len);
    s_qq.ws_assembly.len += (size_t)event->data_len;
    s_qq.ws_assembly.buf[s_qq.ws_assembly.len] = '\0';
    if ((int)s_qq.ws_assembly.len < s_qq.ws_assembly.payload_len) {
        return ESP_OK;
    }
    if ((int)s_qq.ws_assembly.len != s_qq.ws_assembly.payload_len) {
        cap_im_qq_reset_ws_assembly();
        return ESP_FAIL;
    }

    {
        esp_err_t err = cap_im_qq_queue_inbound_frame(s_qq.ws_assembly.buf, s_qq.ws_assembly.len);

        cap_im_qq_reset_ws_assembly();
        return err;
    }
}

static esp_err_t cap_im_qq_http_event_handler(esp_http_client_event_t *event)
{
    cap_im_qq_http_resp_t *resp = (cap_im_qq_http_resp_t *)event->user_data;

    if (!resp || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    if (resp->len + (size_t)event->data_len + 1 > resp->cap) {
        char *tmp = NULL;
        size_t new_cap = resp->cap * 2;

        if (new_cap < resp->len + (size_t)event->data_len + 1) {
            new_cap = resp->len + (size_t)event->data_len + 1;
        }

        tmp = realloc(resp->buf, new_cap);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        resp->buf = tmp;
        resp->cap = new_cap;
    }

    memcpy(resp->buf + resp->len, event->data, event->data_len);
    resp->len += (size_t)event->data_len;
    resp->buf[resp->len] = '\0';
    return ESP_OK;
}

static char *cap_im_qq_make_auth_header(void)
{
    char *auth = NULL;
    int needed;

    needed = snprintf(NULL, 0, "QQBot %s", s_qq.access_token);
    if (needed < 0) {
        return NULL;
    }

    auth = calloc(1, (size_t)needed + 1);
    if (!auth) {
        return NULL;
    }

    snprintf(auth, (size_t)needed + 1, "QQBot %s", s_qq.access_token);
    return auth;
}

static void cap_im_qq_invalidate_token(void)
{
    s_qq.access_token[0] = '\0';
    s_qq.token_expire_time = 0;
}

static bool cap_im_qq_is_token_invalid_response(const char *body)
{
    cJSON *root = NULL;
    cJSON *code_json = NULL;
    int code = 0;
    bool invalid = false;

    if (!body || !body[0]) {
        return false;
    }

    root = cJSON_Parse(body);
    if (!root) {
        return false;
    }

    code_json = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(code_json)) {
        code_json = cJSON_GetObjectItem(root, "err_code");
    }
    if (cJSON_IsNumber(code_json)) {
        code = code_json->valueint;
        invalid = (code == 11244);
    }

    cJSON_Delete(root);
    return invalid;
}

static void cap_im_qq_log_http_failure(const char *stage,
                                       esp_err_t err,
                                       int status,
                                       const char *body)
{
    char body_snippet[161];

    if (!body || !body[0]) {
        body_snippet[0] = '\0';
    } else {
        snprintf(body_snippet, sizeof(body_snippet), "%.160s", body);
    }

    ESP_LOGW(TAG,
             "%s failed: err=%s http=%d body=%s",
             stage ? stage : "QQ HTTP request",
             esp_err_to_name(err),
             status,
             body_snippet);
}

static esp_err_t cap_im_qq_get_access_token(void)
{
    cJSON *body = NULL;
    char *json_str = NULL;
    cap_im_qq_http_resp_t resp = {0};
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status;
    int64_t now = esp_timer_get_time() / 1000000LL;

    if (s_qq.app_id[0] == '\0' || s_qq.app_secret[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_qq.access_token[0] != '\0' && s_qq.token_expire_time > now + 300) {
        return ESP_OK;
    }

    body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "appId", s_qq.app_id);
    cJSON_AddStringToObject(body, "clientSecret", s_qq.app_secret);
    json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    resp.buf = calloc(1, CAP_IM_QQ_HTTP_RESP_INIT);
    resp.cap = CAP_IM_QQ_HTTP_RESP_INIT;
    if (!resp.buf) {
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    config.url = CAP_IM_QQ_TOKEN_URL;
    config.event_handler = cap_im_qq_http_event_handler;
    config.user_data = &resp;
    config.timeout_ms = 15000;
    config.buffer_size = 1024;
#if CONFIG_HTTP_REUSE_ENABLE
    /*
     * With HTTP reuse, keep buffer_size / buffer_size_tx consistent across pooled
     * clients; esp_http_client has no API to resize RX or TX buffers after init.
     */
    config.buffer_size_tx = 2048;
#else
    config.buffer_size_tx = 1024;
#endif
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (!client) {
        free(json_str);
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(json_str);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG,
                 "QQ token request failed: err=%s http=%d body=%s",
                 esp_err_to_name(err),
                 status,
                 resp.buf ? resp.buf : "");
        free(resp.buf);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    {
        cJSON *root = cJSON_Parse(resp.buf);
        cJSON *token_json;
        cJSON *expires_json;
        int expires_in;

        free(resp.buf);
        if (!root) {
            return ESP_FAIL;
        }

        token_json = cJSON_GetObjectItem(root, "access_token");
        expires_json = cJSON_GetObjectItem(root, "expires_in");
        if (!cJSON_IsString(token_json) || !token_json->valuestring) {
            cap_im_qq_log_http_failure("QQ token parse", ESP_FAIL, 200, resp.buf);
            cJSON_Delete(root);
            return ESP_FAIL;
        }

        strlcpy(s_qq.access_token, token_json->valuestring, sizeof(s_qq.access_token));
        expires_in = cJSON_IsNumber(expires_json) ? expires_json->valueint : 7200;
        s_qq.token_expire_time = now + expires_in - 300;
        cJSON_Delete(root);
    }

    return ESP_OK;
}

static esp_err_t cap_im_qq_fetch_gateway_url(void)
{
    int attempt = 0;

retry:
    cap_im_qq_http_resp_t resp = {0};
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    char *auth = NULL;
    esp_err_t err;
    int status;

    err = cap_im_qq_get_access_token();
    if (err != ESP_OK) {
        return err;
    }

    resp.buf = calloc(1, CAP_IM_QQ_HTTP_RESP_INIT);
    resp.cap = CAP_IM_QQ_HTTP_RESP_INIT;
    if (!resp.buf) {
        return ESP_ERR_NO_MEM;
    }

    config.url = CAP_IM_QQ_GATEWAY_URL;
    config.event_handler = cap_im_qq_http_event_handler;
    config.user_data = &resp;
    config.timeout_ms = 10000;
    config.buffer_size = 1024;
#if CONFIG_HTTP_REUSE_ENABLE
    /*
     * With HTTP reuse, keep buffer_size / buffer_size_tx consistent across pooled
     * clients; esp_http_client has no API to resize RX or TX buffers after init.
     */
    config.buffer_size_tx = 2048;
#endif
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return ESP_FAIL;
    }

    auth = cap_im_qq_make_auth_header();
    if (!auth) {
        esp_http_client_cleanup(client);
        free(resp.buf);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Authorization", auth);
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(auth);

    if (err != ESP_OK || status != 200) {
        if (attempt == 0 && cap_im_qq_is_token_invalid_response(resp.buf)) {
            cap_im_qq_invalidate_token();
            free(resp.buf);
            attempt++;
            goto retry;
        }
        cap_im_qq_log_http_failure("QQ gateway fetch", err, status, resp.buf);
        free(resp.buf);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    {
        cJSON *root = cJSON_Parse(resp.buf);
        cJSON *url_json;

        free(resp.buf);
        if (!root) {
            return ESP_FAIL;
        }

        url_json = cJSON_GetObjectItem(root, "url");
        if (!cJSON_IsString(url_json) || !url_json->valuestring) {
            cap_im_qq_log_http_failure("QQ gateway parse", ESP_FAIL, 200, resp.buf);
            cJSON_Delete(root);
            return ESP_FAIL;
        }

        strlcpy(s_qq.ws_url, url_json->valuestring, sizeof(s_qq.ws_url));
        cJSON_Delete(root);
    }

    return ESP_OK;
}

static esp_err_t cap_im_qq_ws_send_json(const char *json_str)
{
    int len;
    int sent;

    if (!s_qq.ws_client || !json_str) {
        return ESP_ERR_INVALID_STATE;
    }

    len = (int)strlen(json_str);
    sent = esp_websocket_client_send_text(s_qq.ws_client, json_str, len, pdMS_TO_TICKS(1000));
    return sent == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t cap_im_qq_ws_send_identify(void)
{
    cJSON *root = NULL;
    cJSON *data = NULL;
    char *json_str = NULL;
    char *auth = NULL;
    esp_err_t err;

    auth = cap_im_qq_make_auth_header();
    if (!auth) {
        return ESP_ERR_NO_MEM;
    }

    root = cJSON_CreateObject();
    data = cJSON_CreateObject();
    if (!root || !data) {
        free(auth);
        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "op", CAP_IM_QQ_WS_OP_IDENTIFY);
    cJSON_AddStringToObject(data, "token", auth);
    cJSON_AddNumberToObject(data, "intents", CAP_IM_QQ_INTENTS);
    cJSON_AddItemToObject(root, "d", data);
    json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(auth);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_qq_ws_send_json(json_str);
    free(json_str);
    return err;
}

static esp_err_t cap_im_qq_ws_send_heartbeat(void)
{
    cJSON *root = cJSON_CreateObject();
    char *json_str = NULL;
    esp_err_t err;

    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "op", CAP_IM_QQ_WS_OP_HEARTBEAT);
    if (s_qq.last_seq >= 0) {
        cJSON_AddNumberToObject(root, "d", s_qq.last_seq);
    } else {
        cJSON_AddNullToObject(root, "d");
    }

    json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_qq_ws_send_json(json_str);
    free(json_str);
    return err;
}

static esp_err_t cap_im_qq_publish_inbound_text(const char *chat_id,
                                                const char *sender_id,
                                                const char *message_id,
                                                const char *content)
{
    if (!content || !content[0]) {
        return ESP_OK;
    }

    return claw_event_router_publish_message("qq_gateway",
                                             "qq",
                                             chat_id,
                                             content,
                                             sender_id,
                                             message_id);
}

static esp_err_t cap_im_qq_publish_attachment_event(const char *chat_id,
                                                    const char *sender_id,
                                                    const char *message_id,
                                                    const char *content_type,
                                                    const char *payload_json)
{
    claw_event_t event = {0};

    if (!chat_id || !message_id || !content_type || !payload_json) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(event.source_cap, "qq_gateway", sizeof(event.source_cap));
    strlcpy(event.event_type, "attachment_saved", sizeof(event.event_type));
    strlcpy(event.source_channel, "qq", sizeof(event.source_channel));
    strlcpy(event.chat_id, chat_id, sizeof(event.chat_id));
    if (sender_id && sender_id[0]) {
        strlcpy(event.sender_id, sender_id, sizeof(event.sender_id));
    }
    strlcpy(event.message_id, message_id, sizeof(event.message_id));
    strlcpy(event.content_type, content_type, sizeof(event.content_type));
    event.timestamp_ms = cap_im_qq_now_ms();
    event.session_policy = CLAW_SESSION_POLICY_CHAT;
    snprintf(event.event_id, sizeof(event.event_id), "qq-attach-%" PRId64, event.timestamp_ms);
    event.text = "";
    event.payload_json = (char *)payload_json;
    return claw_event_router_publish(&event);
}

static esp_err_t cap_im_qq_save_attachment(const char *chat_id,
                                           const char *sender_id,
                                           const char *message_id,
                                           const char *attachment_kind,
                                           const char *url,
                                           const char *original_filename,
                                           const char *mime)
{
    char normalized_url[512];
    char saved_dir[CAP_IM_QQ_PATH_BUF_SIZE];
    char saved_name[CAP_IM_QQ_NAME_BUF_SIZE];
    char saved_path[CAP_IM_QQ_PATH_BUF_SIZE];
    const char *extension = NULL;
    const char *content_type = NULL;
    char *payload_json = NULL;
    size_t bytes = 0;
    esp_err_t err;

    if (!s_qq.enable_inbound_attachments || !s_qq.attachment_root_dir[0] ||
            !chat_id || !message_id || !attachment_kind || !url || !url[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!cap_im_attachment_normalize_url(url, normalized_url, sizeof(normalized_url))) {
        return ESP_ERR_INVALID_ARG;
    }

    extension = cap_im_attachment_guess_extension(normalized_url, original_filename, mime);
    err = cap_im_attachment_build_saved_paths(s_qq.attachment_root_dir,
                                              "qq",
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

    err = cap_im_attachment_download_url_to_file(TAG,
                                                 normalized_url,
                                                 saved_path,
                                                 s_qq.max_inbound_file_bytes,
                                                 &bytes);
    if (err != ESP_OK) {
        return err;
    }

    content_type = cap_im_qq_is_image_mime(mime) ? "image" : "file";
    payload_json = cap_im_attachment_build_payload_json(
    &(cap_im_attachment_payload_config_t) {
        .platform = "qq",
        .attachment_kind = attachment_kind,
        .saved_path = saved_path,
        .saved_dir = saved_dir,
        .saved_name = saved_name,
        .original_filename = original_filename,
        .mime = mime,
        .caption = "",
        .source_key = "attachment_url",
        .source_value = normalized_url,
        .size_bytes = bytes,
        .saved_at_ms = cap_im_qq_now_ms(),
    });
    if (!payload_json) {
        ESP_LOGW(TAG, "QQ attachment payload build failed: message=%s path=%s", message_id, saved_path);
        ESP_LOGI(TAG, "Saved QQ %s to %s (%u bytes)", attachment_kind, saved_path, (unsigned int)bytes);
        return ESP_OK;
    }

    err = cap_im_qq_publish_attachment_event(chat_id,
                                             sender_id,
                                             message_id,
                                             content_type,
                                             payload_json);
    free(payload_json);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "QQ attachment publish event failed: message=%s path=%s err=%s",
                 message_id,
                 saved_path,
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Saved QQ %s to %s (%u bytes)", attachment_kind, saved_path, (unsigned int)bytes);
    return ESP_OK;
}

static void cap_im_qq_handle_attachments(cJSON *attachments,
                                         const char *chat_id,
                                         const char *sender_id,
                                         const char *message_id)
{
    cJSON *item = NULL;

    if (!s_qq.enable_inbound_attachments) {
        ESP_LOGI(TAG, "QQ attachments ignored for %s: inbound attachments disabled", message_id);
        return;
    }
    if (!cJSON_IsArray(attachments)) {
        ESP_LOGI(TAG, "QQ message %s has no attachment array", message_id);
        return;
    }

    ESP_LOGI(TAG,
             "QQ message %s attachment_count=%d chat=%s",
             message_id,
             cJSON_GetArraySize(attachments),
             chat_id ? chat_id : "");

    cJSON_ArrayForEach(item, attachments) {
        const char *url = NULL;
        const char *filename = NULL;
        const char *mime = NULL;
        const char *kind = "file";
        cJSON *url_json = NULL;
        cJSON *filename_json = NULL;
        cJSON *mime_json = NULL;

        if (!cJSON_IsObject(item)) {
            continue;
        }

        url_json = cJSON_GetObjectItem(item, "url");
        filename_json = cJSON_GetObjectItem(item, "filename");
        mime_json = cJSON_GetObjectItem(item, "content_type");
        url = cJSON_IsString(url_json) ? url_json->valuestring : NULL;
        filename = cJSON_IsString(filename_json) ? filename_json->valuestring : cap_im_qq_basename(url);
        mime = cJSON_IsString(mime_json) ? mime_json->valuestring : "application/octet-stream";
        if (!url || !url[0]) {
            ESP_LOGW(TAG, "QQ attachment missing url for message %s", message_id);
            continue;
        }
        if (cap_im_qq_is_image_mime(mime)) {
            kind = "image";
        } else if (cap_im_qq_is_audio_mime(mime)) {
            kind = "file";
        }

        {
            esp_err_t err = cap_im_qq_save_attachment(chat_id,
                                                      sender_id,
                                                      message_id,
                                                      kind,
                                                      url,
                                                      filename,
                                                      mime);
            if (err != ESP_OK) {
                ESP_LOGW(TAG,
                         "Failed to save QQ attachment message=%s err=%s url=%s",
                         message_id,
                         esp_err_to_name(err),
                         url);
            }
        }
    }
}

static void cap_im_qq_handle_attachment_field(cJSON *field,
                                              const char *chat_id,
                                              const char *sender_id,
                                              const char *message_id)
{
    if (cJSON_IsArray(field)) {
        cap_im_qq_handle_attachments(field, chat_id, sender_id, message_id);
    } else if (cJSON_IsObject(field)) {
        cJSON *array = cJSON_CreateArray();

        if (!array) {
            return;
        }
        cJSON_AddItemReferenceToArray(array, field);
        cap_im_qq_handle_attachments(array, chat_id, sender_id, message_id);
        cJSON_Delete(array);
    }
}

static void cap_im_qq_handle_all_media(cJSON *data,
                                       const char *chat_id,
                                       const char *sender_id,
                                       const char *message_id)
{
    static const char *const media_fields[] = {
        "attachments",
        "audio",
        "audios",
        "voice",
        "voices",
        "record",
        "records",
        "file",
        "files",
        "video",
        "videos",
    };
    size_t i;

    if (!cJSON_IsObject(data)) {
        return;
    }

    for (i = 0; i < sizeof(media_fields) / sizeof(media_fields[0]); i++) {
        cJSON *field = cJSON_GetObjectItem(data, media_fields[i]);

        if (!field) {
            continue;
        }
        cap_im_qq_handle_attachment_field(field, chat_id, sender_id, message_id);
    }
}

static void cap_im_qq_handle_dispatch(cJSON *data, const char *event_type)
{
    char chat_id[96] = {0};
    char sender_id[96] = {0};
    const char *content = NULL;
    const char *message_id = NULL;

    if (!data || !event_type) {
        return;
    }

    if (strcmp(event_type, "C2C_MESSAGE_CREATE") == 0) {
        cJSON *author = cJSON_GetObjectItem(data, "author");
        cJSON *openid = author ? cJSON_GetObjectItem(author, "user_openid") : NULL;
        cJSON *content_json = cJSON_GetObjectItem(data, "content");
        cJSON *id_json = cJSON_GetObjectItem(data, "id");

        if (!cJSON_IsString(openid) || !cJSON_IsString(id_json)) {
            return;
        }
        snprintf(chat_id, sizeof(chat_id), "c2c:%s", openid->valuestring);
        strlcpy(sender_id, openid->valuestring, sizeof(sender_id));
        content = cJSON_IsString(content_json) ? content_json->valuestring : "";
        message_id = id_json->valuestring;
    } else if (strcmp(event_type, "GROUP_AT_MESSAGE_CREATE") == 0) {
        cJSON *group = cJSON_GetObjectItem(data, "group_openid");
        cJSON *author = cJSON_GetObjectItem(data, "author");
        cJSON *member = author ? cJSON_GetObjectItem(author, "member_openid") : NULL;
        cJSON *content_json = cJSON_GetObjectItem(data, "content");
        cJSON *id_json = cJSON_GetObjectItem(data, "id");

        if (!cJSON_IsString(group) || !cJSON_IsString(id_json)) {
            return;
        }
        snprintf(chat_id, sizeof(chat_id), "group:%s", group->valuestring);
        if (cJSON_IsString(member)) {
            strlcpy(sender_id, member->valuestring, sizeof(sender_id));
        }
        content = cJSON_IsString(content_json) ? content_json->valuestring : "";
        message_id = id_json->valuestring;
    } else {
        ESP_LOGI(TAG, "QQ dispatch type %s is not handled", event_type);
        return;
    }

    if (cap_im_qq_dedup_check_and_record(message_id)) {
        return;
    }

    cap_im_qq_handle_all_media(data, chat_id, sender_id, message_id);

    if (content && content[0]) {
        if (cap_im_qq_publish_inbound_text(chat_id, sender_id, message_id, content) == ESP_OK) {
            ESP_LOGI(TAG, "QQ inbound %s: %.48s%s", chat_id, content, strlen(content) > 48 ? "..." : "");
        } else {
            ESP_LOGW(TAG, "Failed to publish QQ inbound message");
        }
    }
}

static void cap_im_qq_log_stack_watermark(const char *label)
{
    UBaseType_t words = uxTaskGetStackHighWaterMark(NULL);

    ESP_LOGD(TAG, "%s stack_high_water=%u bytes", label, (unsigned int)(words * sizeof(StackType_t)));
}

static void cap_im_qq_process_frame(const char *frame, size_t frame_len)
{
    cJSON *root;
    cJSON *op_json;
    cJSON *data_json;
    cJSON *seq_json;
    cJSON *type_json;
    int op;
    const char *dispatch_type;

    root = cJSON_ParseWithLength(frame, frame_len);
    if (!root) {
        ESP_LOGW(TAG, "QQ frame parse failed len=%u", (unsigned int)frame_len);
        return;
    }

    op_json = cJSON_GetObjectItem(root, "op");
    data_json = cJSON_GetObjectItem(root, "d");
    seq_json = cJSON_GetObjectItem(root, "s");
    type_json = cJSON_GetObjectItem(root, "t");

    if (cJSON_IsNumber(seq_json)) {
        s_qq.last_seq = seq_json->valueint;
    }

    op = cJSON_IsNumber(op_json) ? op_json->valueint : -1;
    dispatch_type = cJSON_IsString(type_json) ? type_json->valuestring : "";

    switch (op) {
    case CAP_IM_QQ_WS_OP_HELLO:
        if (cJSON_IsObject(data_json)) {
            cJSON *heartbeat_json = cJSON_GetObjectItem(data_json, "heartbeat_interval");

            if (cJSON_IsNumber(heartbeat_json)) {
                s_qq.heartbeat_interval_ms = heartbeat_json->valueint;
            }
        }
        s_qq.ws_identify_pending = true;
        s_qq.ws_connected = true;
        break;
    case CAP_IM_QQ_WS_OP_DISPATCH:
        if (strcmp(dispatch_type, "READY") == 0) {
            ESP_LOGI(TAG, "QQ gateway ready");
        } else {
            cap_im_qq_handle_dispatch(data_json, dispatch_type);
        }
        break;
    case CAP_IM_QQ_WS_OP_RECONNECT:
    case CAP_IM_QQ_WS_OP_INVALID_SESSION:
        s_qq.ws_should_reconnect = true;
        break;
    case CAP_IM_QQ_WS_OP_HEARTBEAT_ACK:
    default:
        break;
    }

    cJSON_Delete(root);
}

static void cap_im_qq_inbound_task(void *arg)
{
    cap_im_qq_inbound_frame_t item = {0};

    (void)arg;

    while (1) {
        if (xQueueReceive(s_qq.inbound_queue, &item, pdMS_TO_TICKS(200)) == pdTRUE) {
            cap_im_qq_process_frame(item.frame, item.len);
            free(item.frame);
            item.frame = NULL;
            cap_im_qq_log_stack_watermark("qq_inbound");
            continue;
        }

        if (s_qq.stop_requested) {
            break;
        }
    }

    s_qq.inbound_task = NULL;
    claw_task_delete(NULL);
}

static void cap_im_qq_ws_event_handler(void *arg,
                                       esp_event_base_t base,
                                       int32_t event_id,
                                       void *event_data)
{
    esp_websocket_event_data_t *event = (esp_websocket_event_data_t *)event_data;

    (void)arg;
    (void)base;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_qq.ws_connected = true;
        s_qq.ws_should_reconnect = false;
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_qq.ws_connected = false;
        s_qq.ws_identify_pending = false;
        s_qq.ws_should_reconnect = true;
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || !event ||
            event->op_code != 0x01 || !event->data_ptr || event->data_len <= 0) {
        return;
    }

    if (s_qq.inbound_queue) {
        esp_err_t err = cap_im_qq_handle_inbound_ws_data(event);

        if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG,
                     "QQ inbound WS frame handling failed len=%d payload_len=%d offset=%d err=%s",
                     event->data_len,
                     event->payload_len,
                     event->payload_offset,
                     esp_err_to_name(err));
        }
    }
}

static esp_err_t cap_im_qq_api_post(const char *path,
                                    const char *body_json,
                                    char **out_response)
{
    int attempt = 0;

retry:
    cap_im_qq_http_resp_t resp = {0};
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    char *url = NULL;
    char *auth = NULL;
    esp_err_t err;
    int needed;
    int status;

    err = cap_im_qq_get_access_token();
    if (err != ESP_OK) {
        return err;
    }

    needed = snprintf(NULL, 0, "%s%s", CAP_IM_QQ_API_BASE, path);
    if (needed < 0) {
        return ESP_FAIL;
    }

    url = calloc(1, (size_t)needed + 1);
    resp.buf = calloc(1, 4096);
    auth = cap_im_qq_make_auth_header();
    if (!url || !resp.buf || !auth) {
        free(url);
        free(resp.buf);
        free(auth);
        return ESP_ERR_NO_MEM;
    }
    resp.cap = 4096;
    snprintf(url, (size_t)needed + 1, "%s%s", CAP_IM_QQ_API_BASE, path);

    config.url = url;
    config.event_handler = cap_im_qq_http_event_handler;
    config.user_data = &resp;
    config.timeout_ms = 15000;
    config.buffer_size = 1024;
    config.buffer_size_tx = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (!client) {
        free(url);
        free(resp.buf);
        free(auth);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (body_json) {
        esp_http_client_set_post_field(client, body_json, strlen(body_json));
    }

    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(url);
    free(auth);

    if (err != ESP_OK) {
        free(resp.buf);
        return err;
    }

    if (status < 200 || status >= 300) {
        if (attempt == 0 && cap_im_qq_is_token_invalid_response(resp.buf)) {
            cap_im_qq_invalidate_token();
            free(resp.buf);
            attempt++;
            goto retry;
        }
        if (out_response) {
            *out_response = resp.buf;
        } else {
            free(resp.buf);
        }
        return ESP_FAIL;
    }

    if (out_response) {
        *out_response = resp.buf;
    } else {
        free(resp.buf);
    }
    return ESP_OK;
}

static char *cap_im_qq_build_chat_path(const char *chat_id, const char *suffix)
{
    const char *format = NULL;
    size_t prefix_len = 0;
    char *path = NULL;
    int needed;

    if (!chat_id || !suffix) {
        return NULL;
    }

    if (strncmp(chat_id, "c2c:", 4) == 0) {
        format = "/v2/users/%s/%s";
        prefix_len = 4;
    } else if (strncmp(chat_id, "group:", 6) == 0) {
        format = "/v2/groups/%s/%s";
        prefix_len = 6;
    } else {
        return NULL;
    }

    needed = snprintf(NULL, 0, format, chat_id + prefix_len, suffix);
    if (needed < 0) {
        return NULL;
    }

    path = calloc(1, (size_t)needed + 1);
    if (!path) {
        return NULL;
    }

    snprintf(path, (size_t)needed + 1, format, chat_id + prefix_len, suffix);
    return path;
}

static char *cap_im_qq_build_message_path(const char *chat_id)
{
    return cap_im_qq_build_chat_path(chat_id, "messages");
}

static char *cap_im_qq_build_file_path(const char *chat_id)
{
    return cap_im_qq_build_chat_path(chat_id, "files");
}

static char *cap_im_qq_file_to_base64(const char *path)
{
    struct stat st;
    FILE *file = NULL;
    unsigned char *raw_buf = NULL;
    unsigned char *encoded_buf = NULL;
    size_t raw_size;
    size_t read_size;
    size_t encoded_len = 0;

    if (!path || stat(path, &st) != 0) {
        return NULL;
    }
    if (st.st_size <= 0) {
        return NULL;
    }

    raw_size = (size_t)st.st_size;
    file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }

    raw_buf = malloc(raw_size);
    if (!raw_buf) {
        fclose(file);
        return NULL;
    }

    read_size = fread(raw_buf, 1, raw_size, file);
    fclose(file);
    if (read_size != raw_size) {
        free(raw_buf);
        return NULL;
    }

    if (mbedtls_base64_encode(NULL, 0, &encoded_len, raw_buf, read_size) !=
            MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        free(raw_buf);
        return NULL;
    }

    encoded_buf = malloc(encoded_len + 1);
    if (!encoded_buf) {
        free(raw_buf);
        return NULL;
    }

    if (mbedtls_base64_encode(encoded_buf, encoded_len, &encoded_len, raw_buf, read_size) != 0) {
        free(raw_buf);
        free(encoded_buf);
        return NULL;
    }

    free(raw_buf);
    encoded_buf[encoded_len] = '\0';
    return (char *)encoded_buf;
}

static esp_err_t cap_im_qq_send_media_message(const char *chat_id,
                                              const char *file_info,
                                              const char *caption)
{
    cJSON *body = NULL;
    cJSON *media = NULL;
    char *json_str = NULL;
    char *path = NULL;
    esp_err_t err;

    if (!chat_id || !file_info || !file_info[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(body, "msg_type", 7);
    media = cJSON_CreateObject();
    if (!media) {
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(media, "file_info", file_info);
    cJSON_AddItemToObject(body, "media", media);
    if (caption && caption[0]) {
        cJSON_AddStringToObject(body, "content", caption);
    }

    json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    path = cap_im_qq_build_message_path(chat_id);
    if (!path) {
        free(json_str);
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_qq_api_post(path, json_str, NULL);
    free(path);
    free(json_str);
    return err;
}

static esp_err_t cap_im_qq_upload_media(const char *chat_id,
                                        const char *path,
                                        uint32_t file_type,
                                        char **out_file_info)
{
    struct stat st;
    const char *base_name = NULL;
    cJSON *body = NULL;
    char *path_buf = NULL;
    char *file_b64 = NULL;
    char *json_str = NULL;
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *file_info_json = NULL;
    esp_err_t err;

    if (!chat_id || !path || !path[0] || !out_file_info) {
        return ESP_ERR_INVALID_ARG;
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

    file_b64 = cap_im_qq_file_to_base64(path);
    if (!file_b64) {
        ESP_LOGE(TAG, "Failed to encode QQ media file: %s", path);
        return ESP_FAIL;
    }

    body = cJSON_CreateObject();
    if (!body) {
        free(file_b64);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(body, "file_type", (double)file_type);
    cJSON_AddBoolToObject(body, "srv_send_msg", false);
    cJSON_AddStringToObject(body, "file_data", file_b64);
    if (file_type == CAP_IM_QQ_FILE_TYPE_FILE) {
        base_name = cap_im_qq_basename(path);
        if (base_name && base_name[0]) {
            cJSON_AddStringToObject(body, "file_name", base_name);
        }
    }
    json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    free(file_b64);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    path_buf = cap_im_qq_build_file_path(chat_id);
    if (!path_buf) {
        free(json_str);
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_qq_api_post(path_buf, json_str, &response);
    free(path_buf);
    free(json_str);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QQ media upload failed for %s: %s", chat_id, esp_err_to_name(err));
        free(response);
        return err;
    }

    root = cJSON_Parse(response);
    free(response);
    if (!root) {
        ESP_LOGE(TAG, "QQ media upload response parse failed");
        return ESP_FAIL;
    }

    file_info_json = cJSON_GetObjectItem(root, "file_info");
    if (!cJSON_IsString(file_info_json) || !file_info_json->valuestring ||
            !file_info_json->valuestring[0]) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "QQ media upload response missing file_info");
        return ESP_FAIL;
    }

    *out_file_info = strdup(file_info_json->valuestring);
    cJSON_Delete(root);
    return *out_file_info ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t cap_im_qq_send_media(const char *chat_id,
                                      const char *path,
                                      const char *caption,
                                      uint32_t file_type,
                                      const char *kind)
{
    char *file_info = NULL;
    char *resolved_caption = NULL;
    esp_err_t err;

    if (!chat_id || !path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_qq.app_id[0] == '\0' || s_qq.app_secret[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    err = cap_im_qq_upload_media(chat_id, path, file_type, &file_info);
    if (err != ESP_OK) {
        return err;
    }

    if (file_type == CAP_IM_QQ_FILE_TYPE_FILE) {
        const char *base_name = cap_im_qq_basename(path);

        if (caption && caption[0]) {
            int needed = snprintf(NULL, 0, "%s\nfilename: %s", caption, base_name);

            if (needed < 0) {
                free(file_info);
                return ESP_FAIL;
            }
            resolved_caption = calloc(1, (size_t)needed + 1);
            if (!resolved_caption) {
                free(file_info);
                return ESP_ERR_NO_MEM;
            }
            snprintf(resolved_caption, (size_t)needed + 1, "%s\nfilename: %s", caption, base_name);
        } else if (base_name && base_name[0]) {
            int needed = snprintf(NULL, 0, "filename: %s", base_name);

            if (needed < 0) {
                free(file_info);
                return ESP_FAIL;
            }
            resolved_caption = calloc(1, (size_t)needed + 1);
            if (!resolved_caption) {
                free(file_info);
                return ESP_ERR_NO_MEM;
            }
            snprintf(resolved_caption, (size_t)needed + 1, "filename: %s", base_name);
        }
    }

    err = cap_im_qq_send_media_message(chat_id,
                                       file_info,
                                       resolved_caption ? resolved_caption : caption);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QQ %s send failed for %s: %s", kind, chat_id, esp_err_to_name(err));
        free(resolved_caption);
        free(file_info);
        return err;
    }

    ESP_LOGI(TAG, "QQ %s send success to %s: %s", kind, chat_id, path);
    free(resolved_caption);
    free(file_info);
    return ESP_OK;
}

static esp_err_t cap_im_qq_send_message_chunk(const char *chat_id, const char *message)
{
    cJSON *body = cJSON_CreateObject();
    char *json_str = NULL;
    char *path = NULL;
    esp_err_t err;

    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    if (s_qq.msg_type == 2) {
        cJSON *markdown = cJSON_CreateObject();
        if (!markdown) {
            cJSON_Delete(body);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(markdown, "content", message);
        cJSON_AddItemToObject(body, "markdown", markdown);
    } else {
        cJSON_AddStringToObject(body, "content", message);
    }
    cJSON_AddNumberToObject(body, "msg_type", s_qq.msg_type);
    json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    path = cap_im_qq_build_message_path(chat_id);
    if (!path) {
        free(json_str);
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_qq_api_post(path, json_str, NULL);
    free(json_str);
    free(path);
    return err;
}

static esp_err_t cap_im_qq_gateway_init(void)
{
    if (s_qq.app_id[0] == '\0' || s_qq.app_secret[0] == '\0') {
        ESP_LOGW(TAG, "QQ credentials not configured");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "QQ configured (app_id=%.8s...)", s_qq.app_id);
    return ESP_OK;
}

static void cap_im_qq_ws_task(void *arg)
{
    (void)arg;

    while (!s_qq.stop_requested) {
        esp_websocket_client_config_t ws_config = {0};
        int64_t connect_start_ms;
        int64_t last_heartbeat_ms = 0;

        if (cap_im_qq_fetch_gateway_url() != ESP_OK) {
            if (s_qq.stop_requested) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(CAP_IM_QQ_RECONNECT_DELAY_MS));
            continue;
        }

        ws_config.uri = s_qq.ws_url;
        ws_config.buffer_size = 1024;
        ws_config.task_stack = CAP_IM_QQ_WS_CLIENT_STACK;
        ws_config.task_prio = CAP_IM_QQ_WS_PRIO;
        ws_config.reconnect_timeout_ms = CAP_IM_QQ_RECONNECT_DELAY_MS;
        ws_config.network_timeout_ms = 10000;
        ws_config.disable_auto_reconnect = true;
        ws_config.crt_bundle_attach = esp_crt_bundle_attach;

        s_qq.ws_client = esp_websocket_client_init(&ws_config);
        if (!s_qq.ws_client) {
            if (s_qq.stop_requested) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(CAP_IM_QQ_RECONNECT_DELAY_MS));
            continue;
        }

        s_qq.last_seq = -1;
        s_qq.ws_connected = false;
        s_qq.ws_identify_pending = false;
        s_qq.ws_should_reconnect = false;
        esp_websocket_register_events(s_qq.ws_client,
                                      WEBSOCKET_EVENT_ANY,
                                      cap_im_qq_ws_event_handler,
                                      NULL);
        esp_websocket_client_start(s_qq.ws_client);
        connect_start_ms = esp_timer_get_time() / 1000LL;

        while (s_qq.ws_client && !s_qq.stop_requested) {
            int64_t now_ms = esp_timer_get_time() / 1000LL;

            if (s_qq.ws_identify_pending) {
                if (cap_im_qq_ws_send_identify() == ESP_OK) {
                    s_qq.ws_identify_pending = false;
                    last_heartbeat_ms = now_ms;
                } else {
                    s_qq.ws_should_reconnect = true;
                }
            } else if (s_qq.ws_connected &&
                       now_ms - last_heartbeat_ms >= s_qq.heartbeat_interval_ms) {
                if (cap_im_qq_ws_send_heartbeat() != ESP_OK) {
                    s_qq.ws_should_reconnect = true;
                }
                last_heartbeat_ms = now_ms;
            }

            if (s_qq.ws_should_reconnect) {
                break;
            }
            if (!esp_websocket_client_is_connected(s_qq.ws_client) && !s_qq.ws_connected &&
                    now_ms - connect_start_ms >= CAP_IM_QQ_WS_CONNECT_GRACE_MS) {
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(200));
        }

        esp_websocket_client_stop(s_qq.ws_client);
        esp_websocket_client_destroy(s_qq.ws_client);
        s_qq.ws_client = NULL;
        s_qq.ws_connected = false;
        s_qq.ws_identify_pending = false;
        s_qq.ws_should_reconnect = false;
        if (s_qq.stop_requested) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(CAP_IM_QQ_RECONNECT_DELAY_MS));
    }

    s_qq.ws_task = NULL;
    s_qq.ws_client = NULL;
    claw_task_delete(NULL);
}

static void cap_im_qq_reset_runtime_state(void)
{
    cap_im_qq_reset_ws_assembly();
    s_qq.ws_client = NULL;
    s_qq.ws_task = NULL;
    s_qq.inbound_task = NULL;
    s_qq.ws_connected = false;
    s_qq.ws_identify_pending = false;
    s_qq.ws_should_reconnect = false;
    s_qq.stop_requested = false;
    s_qq.last_seq = -1;
}

static esp_err_t cap_im_qq_gateway_start(void)
{
    BaseType_t ok;

    if (s_qq.app_id[0] == '\0' || s_qq.app_secret[0] == '\0') {
        ESP_LOGW(TAG, "QQ not configured, skipping gateway start");
        return ESP_OK;
    }
    if (s_qq.ws_task) {
        return ESP_OK;
    }
    s_qq.stop_requested = false;
    if (!s_qq.inbound_queue) {
        s_qq.inbound_queue = xQueueCreate(CAP_IM_QQ_INBOUND_QUEUE_LEN,
                                          sizeof(cap_im_qq_inbound_frame_t));
        if (!s_qq.inbound_queue) {
            return ESP_ERR_NO_MEM;
        }
    }

    ok = claw_task_create(&(claw_task_config_t){
                              .name = "qq_inbound",
                              .stack_size = 8192,
                              .priority = 5,
                              .core_id = tskNO_AFFINITY,
                              .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                          },
                          cap_im_qq_inbound_task,
                          NULL,
                          &s_qq.inbound_task);
    if (ok != pdPASS) {
        vQueueDelete(s_qq.inbound_queue);
        s_qq.inbound_queue = NULL;
        s_qq.inbound_task = NULL;
        return ESP_FAIL;
    }

    ok = claw_task_create(&(claw_task_config_t){
                              .name = "qq_ws",
                              .stack_size = 6144,
                              .priority = 5,
                              .core_id = tskNO_AFFINITY,
                              .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                          },
                          cap_im_qq_ws_task,
                          NULL,
                          &s_qq.ws_task);
    if (ok != pdPASS) {
        s_qq.stop_requested = true;
        s_qq.ws_task = NULL;
        while (s_qq.inbound_task) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vQueueDelete(s_qq.inbound_queue);
        s_qq.inbound_queue = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_im_qq_gateway_stop(void)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);

    if (!s_qq.ws_task) {
        cap_im_qq_reset_runtime_state();
        return ESP_OK;
    }

    s_qq.stop_requested = true;
    s_qq.ws_should_reconnect = true;
    if (s_qq.ws_client) {
        esp_websocket_client_stop(s_qq.ws_client);
    }

    while (s_qq.ws_task && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_qq.ws_task) {
        return ESP_ERR_TIMEOUT;
    }

    while (s_qq.inbound_task && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_qq.inbound_task) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_qq.inbound_queue) {
        cap_im_qq_inbound_frame_t item = {0};

        while (xQueueReceive(s_qq.inbound_queue, &item, 0) == pdTRUE) {
            free(item.frame);
        }
        vQueueDelete(s_qq.inbound_queue);
        s_qq.inbound_queue = NULL;
    }

    cap_im_qq_reset_runtime_state();
    return ESP_OK;
}

static esp_err_t cap_im_qq_send_message_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    cJSON *root = NULL;
    cJSON *chat_id_json;
    cJSON *message_json;
    const char *chat_id = NULL;
    const char *message = NULL;
    esp_err_t err;

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    chat_id_json = cJSON_GetObjectItem(root, "chat_id");
    message_json = cJSON_GetObjectItem(root, "message");
    if (cJSON_IsString(chat_id_json) && chat_id_json->valuestring && chat_id_json->valuestring[0]) {
        chat_id = chat_id_json->valuestring;
    } else if (ctx && ctx->chat_id && ctx->chat_id[0]) {
        chat_id = ctx->chat_id;
    }
    if (cJSON_IsString(message_json) && message_json->valuestring && message_json->valuestring[0]) {
        message = message_json->valuestring;
    }

    if (!chat_id || !message) {
        ESP_LOGW(TAG,
                 "QQ outbound invalid args chat_id=%s message_present=%s input=%s",
                 chat_id ? chat_id : "(null)",
                 message ? "true" : "false",
                 input_json ? input_json : "(null)");
        cJSON_Delete(root);
        snprintf(output,
                 output_size,
                 "Error: chat_id and message are required (chat_id may come from ctx)");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "QQ outbound text to %s: %.48s%s",
             chat_id,
             message,
             strlen(message) > 48 ? "..." : "");

    err = cap_im_qq_send_text(chat_id, message);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: %s", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "reply already sent to user");
    return ESP_OK;
}

static esp_err_t cap_im_qq_send_media_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size,
                                              uint32_t file_type,
                                              const char *kind)
{
    cJSON *root = NULL;
    cJSON *chat_id_json;
    cJSON *path_json;
    cJSON *caption_json;
    const char *chat_id = NULL;
    const char *path = NULL;
    const char *caption = NULL;
    esp_err_t err;

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON");
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
                 "Error: chat_id and path are required (chat_id may come from ctx)");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_qq_send_media(chat_id, path, caption, file_type, kind);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: %s", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "reply already sent to user");
    return ESP_OK;
}

static esp_err_t cap_im_qq_send_image_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    return cap_im_qq_send_media_execute(input_json,
                                        ctx,
                                        output,
                                        output_size,
                                        CAP_IM_QQ_FILE_TYPE_IMAGE,
                                        "image");
}

static esp_err_t cap_im_qq_send_file_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    return cap_im_qq_send_media_execute(input_json,
                                        ctx,
                                        output,
                                        output_size,
                                        CAP_IM_QQ_FILE_TYPE_FILE,
                                        "file");
}

static const claw_cap_descriptor_t s_qq_descriptors[] = {
    {
        .id = "qq_gateway",
        .name = "qq_gateway",
        .family = "im",
        .description = "Official QQ Bot gateway event source.",
        .kind = CLAW_CAP_KIND_EVENT_SOURCE,
        .cap_flags = CLAW_CAP_FLAG_EMITS_EVENTS |
        CLAW_CAP_FLAG_SUPPORTS_LIFECYCLE,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .init = cap_im_qq_gateway_init,
        .start = cap_im_qq_gateway_start,
        .stop = cap_im_qq_gateway_stop,
    },
    {
        .id = "qq_send_message",
        .name = "qq_send_message",
        .family = "im",
        .description = "Send a text message to an explicit QQ chat_id.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"}},\"required\":[\"chat_id\",\"message\"]}",
        .execute = cap_im_qq_send_message_execute,
    },
    {
        .id = "qq_send_image",
        .name = "qq_send_image",
        .family = "im",
        .description = "Send an image file from a local path to a QQ chat.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"caption\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        .execute = cap_im_qq_send_image_execute,
    },
    {
        .id = "qq_send_file",
        .name = "qq_send_file",
        .family = "im",
        .description = "Send a file from a local path to a QQ chat.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"caption\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        .execute = cap_im_qq_send_file_execute,
    },
};

static const claw_cap_group_t s_qq_group = {
    .group_id = "cap_im_qq",
    .descriptors = s_qq_descriptors,
    .descriptor_count = sizeof(s_qq_descriptors) / sizeof(s_qq_descriptors[0]),
};

esp_err_t cap_im_qq_register_group(void)
{
    cap_im_qq_init_defaults();

    if (claw_cap_group_exists(s_qq_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_qq_group);
}

void cap_im_qq_set_msg_type(int msg_type)
{
    if (!s_qq_initialized) {
        return;
    }
    s_qq.msg_type = msg_type;
}

esp_err_t cap_im_qq_set_credentials(const char *app_id, const char *app_secret)
{
    if (!s_qq_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!app_id || !app_secret) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_qq.app_id, app_id, sizeof(s_qq.app_id));
    strlcpy(s_qq.app_secret, app_secret, sizeof(s_qq.app_secret));
    s_qq.access_token[0] = '\0';
    s_qq.token_expire_time = 0;
    return ESP_OK;
}

esp_err_t cap_im_qq_set_attachment_config(
    const cap_im_qq_attachment_config_t *config)
{
    if (!s_qq_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    s_qq.enable_inbound_attachments = config->enable_inbound_attachments;
    s_qq.max_inbound_file_bytes = config->max_inbound_file_bytes;
    if (config->storage_root_dir) {
        strlcpy(s_qq.attachment_root_dir,
                config->storage_root_dir,
                sizeof(s_qq.attachment_root_dir));
    } else {
        s_qq.attachment_root_dir[0] = '\0';
    }

    return ESP_OK;
}

esp_err_t cap_im_qq_start(void)
{
    if (!s_qq_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_qq.app_id[0] == '\0' || s_qq.app_secret[0] == '\0') {
        ESP_LOGE(TAG, "QQ credentials are not configured");
        return ESP_ERR_INVALID_STATE;
    }

    return cap_im_qq_gateway_start();
}

esp_err_t cap_im_qq_stop(void)
{
    return cap_im_qq_gateway_stop();
}

esp_err_t cap_im_qq_send_text(const char *chat_id, const char *text)
{
    if (!s_qq_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t text_len;
    size_t offset = 0;
    esp_err_t last_err = ESP_OK;

    if (!chat_id || !text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_qq.app_id[0] == '\0' || s_qq.app_secret[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    text_len = strlen(text);
    while (offset < text_len) {
        size_t chunk_len = text_len - offset;
        char *chunk = NULL;
        esp_err_t err;

        if (chunk_len > CAP_IM_QQ_MAX_MSG_LEN) {
            chunk_len = CAP_IM_QQ_MAX_MSG_LEN;
        }

        chunk = calloc(1, chunk_len + 1);
        if (!chunk) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(chunk, text + offset, chunk_len);

        err = cap_im_qq_send_message_chunk(chat_id, chunk);
        free(chunk);
        if (err != ESP_OK) {
            last_err = err;
        }

        offset += chunk_len;
    }

    return last_err;
}

esp_err_t cap_im_qq_send_image(const char *chat_id, const char *path, const char *caption)
{
    if (!s_qq_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return cap_im_qq_send_media(chat_id,
                                path,
                                caption,
                                CAP_IM_QQ_FILE_TYPE_IMAGE,
                                "image");
}

esp_err_t cap_im_qq_send_file(const char *chat_id, const char *path, const char *caption)
{
    if (!s_qq_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return cap_im_qq_send_media(chat_id,
                                path,
                                caption,
                                CAP_IM_QQ_FILE_TYPE_FILE,
                                "file");
}
