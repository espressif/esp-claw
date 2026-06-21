/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_tg.h"
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
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "cap_im_tg";

#define CAP_IM_TG_API_BASE            "https://api.telegram.org"
#define CAP_IM_TG_HTTP_RESP_INIT      2048
#define CAP_IM_TG_MAX_MSG_LEN         4096
#define CAP_IM_TG_POLL_TIMEOUT_S      20
#define CAP_IM_TG_RETRY_DELAY_MS      3000
#define CAP_IM_TG_ATTACHMENT_QUEUE_LEN 8
#define CAP_IM_TG_DEDUP_CACHE_SIZE    64
#define CAP_IM_TG_PATH_BUF_SIZE       256
#define CAP_IM_TG_NAME_BUF_SIZE       96
#define CAP_IM_TG_MULTIPART_BOUNDARY  "----cap_im_tg_boundary"

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} cap_im_tg_http_resp_t;

typedef struct {
    FILE *file;
    size_t bytes_written;
    size_t max_bytes;
    bool limit_hit;
} cap_im_tg_download_t;

typedef struct {
    char *chat_id;
    char *sender_id;
    char *message_id;
    char *attachment_kind;
    char *file_id;
    char *original_filename;
    char *mime;
    char *caption;
    char *content_type;
} cap_im_tg_attachment_job_t;

typedef struct {
    char bot_token[192];
    char attachment_root_dir[128];
    size_t max_inbound_file_bytes;
    bool enable_inbound_attachments;
    TaskHandle_t poll_task;
    TaskHandle_t attachment_task;
    QueueHandle_t attachment_queue;
    volatile bool stop_requested;
    int64_t next_update_id;
    uint64_t seen_update_keys[CAP_IM_TG_DEDUP_CACHE_SIZE];
    size_t seen_update_idx;
} cap_im_tg_state_t;

static EXT_RAM_BSS_ATTR cap_im_tg_state_t s_tg;
static bool s_tg_initialized;

static void cap_im_tg_init_defaults(void)
{
    if (s_tg_initialized) {
        return;
    }

    s_tg.max_inbound_file_bytes = 2 * 1024 * 1024;
    s_tg.enable_inbound_attachments = false;
    s_tg.next_update_id = 0;
    s_tg_initialized = true;
}

static int64_t cap_im_tg_now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static uint64_t cap_im_tg_fnv1a64(const char *text)
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

static bool cap_im_tg_dedup_check_and_record(const char *update_key)
{
    uint64_t key;
    size_t i;

    if (!update_key || !update_key[0]) {
        return false;
    }

    key = cap_im_tg_fnv1a64(update_key);
    for (i = 0; i < CAP_IM_TG_DEDUP_CACHE_SIZE; i++) {
        if (s_tg.seen_update_keys[i] == key) {
            return true;
        }
    }

    s_tg.seen_update_keys[s_tg.seen_update_idx] = key;
    s_tg.seen_update_idx = (s_tg.seen_update_idx + 1) % CAP_IM_TG_DEDUP_CACHE_SIZE;
    return false;
}

static esp_err_t cap_im_tg_http_event_handler(esp_http_client_event_t *event)
{
    cap_im_tg_http_resp_t *resp = (cap_im_tg_http_resp_t *)event->user_data;

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

static esp_err_t cap_im_tg_api_call(const char *method,
                                    const char *body_json,
                                    char **out_response)
{
    cap_im_tg_http_resp_t resp = {0};
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    char *url = NULL;
    int needed;
    esp_err_t err;
    int status;

    if (!method || !out_response) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tg.bot_token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    needed = snprintf(NULL, 0, "%s/bot%s/%s", CAP_IM_TG_API_BASE, s_tg.bot_token, method);
    if (needed < 0) {
        return ESP_FAIL;
    }

    url = calloc(1, (size_t)needed + 1);
    resp.buf = calloc(1, CAP_IM_TG_HTTP_RESP_INIT);
    if (!url || !resp.buf) {
        free(url);
        free(resp.buf);
        return ESP_ERR_NO_MEM;
    }

    resp.cap = CAP_IM_TG_HTTP_RESP_INIT;
    snprintf(url, (size_t)needed + 1, "%s/bot%s/%s", CAP_IM_TG_API_BASE, s_tg.bot_token, method);

    config.url = url;
    config.event_handler = cap_im_tg_http_event_handler;
    config.user_data = &resp;
    config.timeout_ms = (CAP_IM_TG_POLL_TIMEOUT_S + 5) * 1000;
    config.buffer_size = 1024;
    config.buffer_size_tx = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (!client) {
        free(url);
        free(resp.buf);
        return ESP_FAIL;
    }

    if (body_json) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body_json, strlen(body_json));
    }

    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(url);

    if (err != ESP_OK) {
        free(resp.buf);
        return err;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Telegram API error %d: %s", status, resp.buf ? resp.buf : "");
        free(resp.buf);
        return ESP_FAIL;
    }

    *out_response = resp.buf;
    return ESP_OK;
}

static esp_err_t cap_im_tg_publish_inbound_text(const char *chat_id,
                                                const char *sender_id,
                                                const char *message_id,
                                                const char *content)
{
    if (!content || !content[0]) {
        return ESP_OK;
    }

    return claw_event_router_publish_message("tg_gateway",
                                             "telegram",
                                             chat_id,
                                             content,
                                             sender_id,
                                             message_id);
}

static esp_err_t cap_im_tg_publish_attachment_event(const char *chat_id,
                                                    const char *sender_id,
                                                    const char *message_id,
                                                    const char *content_type,
                                                    const char *payload_json)
{
    claw_event_t event = {0};

    if (!chat_id || !message_id || !content_type || !payload_json) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(event.source_cap, "tg_gateway", sizeof(event.source_cap));
    strlcpy(event.event_type, "attachment_saved", sizeof(event.event_type));
    strlcpy(event.source_channel, "telegram", sizeof(event.source_channel));
    strlcpy(event.chat_id, chat_id, sizeof(event.chat_id));
    if (sender_id && sender_id[0]) {
        strlcpy(event.sender_id, sender_id, sizeof(event.sender_id));
    }
    strlcpy(event.message_id, message_id, sizeof(event.message_id));
    strlcpy(event.content_type, content_type, sizeof(event.content_type));
    event.timestamp_ms = cap_im_tg_now_ms();
    event.session_policy = CLAW_SESSION_POLICY_CHAT;
    snprintf(event.event_id, sizeof(event.event_id), "tg-attach-%" PRId64, event.timestamp_ms);
    event.text = "";
    event.payload_json = (char *)payload_json;
    return claw_event_router_publish(&event);
}

static void cap_im_tg_log_stack_watermark(const char *label)
{
    UBaseType_t words = uxTaskGetStackHighWaterMark(NULL);

    ESP_LOGD(TAG, "%s stack_high_water=%u bytes", label, (unsigned int)(words * sizeof(StackType_t)));
}

static void cap_im_tg_free_attachment_job(cap_im_tg_attachment_job_t *job)
{
    if (!job) {
        return;
    }

    free(job->chat_id);
    free(job->sender_id);
    free(job->message_id);
    free(job->attachment_kind);
    free(job->file_id);
    free(job->original_filename);
    free(job->mime);
    free(job->caption);
    free(job->content_type);
    free(job);
}

static char *cap_im_tg_strdup_or_empty(const char *value)
{
    return strdup(value ? value : "");
}

static cap_im_tg_attachment_job_t *cap_im_tg_make_attachment_job(
    const char *chat_id,
    const char *sender_id,
    const char *message_id,
    const char *attachment_kind,
    const char *file_id,
    const char *original_filename,
    const char *mime,
    const char *caption,
    const char *content_type)
{
    cap_im_tg_attachment_job_t *job = calloc(1, sizeof(*job));

    if (!job) {
        return NULL;
    }

    job->chat_id = cap_im_tg_strdup_or_empty(chat_id);
    job->sender_id = cap_im_tg_strdup_or_empty(sender_id);
    job->message_id = cap_im_tg_strdup_or_empty(message_id);
    job->attachment_kind = cap_im_tg_strdup_or_empty(attachment_kind);
    job->file_id = cap_im_tg_strdup_or_empty(file_id);
    job->original_filename = cap_im_tg_strdup_or_empty(original_filename);
    job->mime = cap_im_tg_strdup_or_empty(mime);
    job->caption = cap_im_tg_strdup_or_empty(caption);
    job->content_type = cap_im_tg_strdup_or_empty(content_type);
    if (!job->chat_id || !job->sender_id || !job->message_id || !job->attachment_kind ||
            !job->file_id || !job->original_filename || !job->mime || !job->caption ||
            !job->content_type) {
        cap_im_tg_free_attachment_job(job);
        return NULL;
    }

    return job;
}

static cJSON *cap_im_tg_select_best_photo(cJSON *message_json)
{
    cJSON *photos = cJSON_GetObjectItem(message_json, "photo");
    cJSON *item = NULL;
    cJSON *selected = NULL;

    if (!cJSON_IsArray(photos)) {
        return NULL;
    }

    cJSON_ArrayForEach(item, photos) {
        selected = item;
    }

    return selected;
}

static char *cap_im_tg_get_file_path(const char *file_id)
{
    char method[256];
    char *resp = NULL;
    cJSON *root = NULL;
    cJSON *ok_json;
    cJSON *result_json;
    cJSON *file_path_json;
    char *copy = NULL;

    if (!file_id || !file_id[0]) {
        return NULL;
    }

    snprintf(method, sizeof(method), "getFile?file_id=%s", file_id);
    if (cap_im_tg_api_call(method, NULL, &resp) != ESP_OK) {
        return NULL;
    }

    root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        return NULL;
    }

    ok_json = cJSON_GetObjectItem(root, "ok");
    result_json = cJSON_GetObjectItem(root, "result");
    file_path_json = cJSON_IsObject(result_json) ? cJSON_GetObjectItem(result_json, "file_path") : NULL;
    if (cJSON_IsTrue(ok_json) && cJSON_IsString(file_path_json) && file_path_json->valuestring) {
        copy = strdup(file_path_json->valuestring);
    }

    cJSON_Delete(root);
    return copy;
}

static esp_err_t cap_im_tg_save_attachment(const char *chat_id,
                                           const char *sender_id,
                                           const char *message_id,
                                           const char *attachment_kind,
                                           const char *file_id,
                                           const char *original_filename,
                                           const char *mime,
                                           const char *caption,
                                           const char *content_type)
{
    char saved_dir[CAP_IM_TG_PATH_BUF_SIZE];
    char saved_name[CAP_IM_TG_NAME_BUF_SIZE];
    char saved_path[CAP_IM_TG_PATH_BUF_SIZE];
    char *remote_path = NULL;
    char download_url[CAP_IM_TG_PATH_BUF_SIZE];
    const char *extension = NULL;
    size_t bytes = 0;
    char *payload_json = NULL;
    esp_err_t err;

    if (!s_tg.enable_inbound_attachments || !s_tg.attachment_root_dir[0] ||
            !chat_id || !message_id || !attachment_kind || !file_id || !content_type) {
        return ESP_ERR_INVALID_STATE;
    }

    remote_path = cap_im_tg_get_file_path(file_id);
    if (!remote_path) {
        return ESP_FAIL;
    }

    extension = cap_im_attachment_guess_extension(remote_path, original_filename, mime);
    err = cap_im_attachment_build_saved_paths(s_tg.attachment_root_dir,
                                              "telegram",
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
        free(remote_path);
        return err;
    }

    snprintf(download_url, sizeof(download_url), "%s/file/bot%s/%s",
             CAP_IM_TG_API_BASE, s_tg.bot_token, remote_path);
    err = cap_im_attachment_download_url_to_file(TAG,
                                                 download_url,
                                                 saved_path,
                                                 s_tg.max_inbound_file_bytes,
                                                 &bytes);
    if (err != ESP_OK) {
        free(remote_path);
        return err;
    }

    payload_json = cap_im_attachment_build_payload_json(
    &(cap_im_attachment_payload_config_t) {
        .platform = "telegram",
        .attachment_kind = attachment_kind,
        .saved_path = saved_path,
        .saved_dir = saved_dir,
        .saved_name = saved_name,
        .original_filename = original_filename,
        .mime = mime,
        .caption = caption,
        .source_key = "platform_file_id",
        .source_value = file_id,
        .size_bytes = bytes,
        .saved_at_ms = cap_im_tg_now_ms(),
    });
    if (!payload_json) {
        free(remote_path);
        ESP_LOGW(TAG, "Telegram attachment payload build failed: message=%s path=%s",
                 message_id, saved_path);
        ESP_LOGI(TAG, "Saved Telegram %s to %s (%u bytes)", attachment_kind, saved_path, (unsigned int)bytes);
        return ESP_OK;
    }

    err = cap_im_tg_publish_attachment_event(chat_id,
                                             sender_id,
                                             message_id,
                                             content_type,
                                             payload_json);
    free(payload_json);
    free(remote_path);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Telegram attachment publish event failed: message=%s path=%s err=%s",
                 message_id,
                 saved_path,
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Saved Telegram %s to %s (%u bytes)", attachment_kind, saved_path, (unsigned int)bytes);
    return ESP_OK;
}

static void cap_im_tg_attachment_task(void *arg)
{
    cap_im_tg_attachment_job_t *job = NULL;

    (void)arg;

    while (1) {
        if (xQueueReceive(s_tg.attachment_queue, &job, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (job) {
                esp_err_t err = cap_im_tg_save_attachment(job->chat_id,
                                                          job->sender_id,
                                                          job->message_id,
                                                          job->attachment_kind,
                                                          job->file_id,
                                                          job->original_filename,
                                                          job->mime,
                                                          job->caption,
                                                          job->content_type);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "Failed to save Telegram %s message=%s err=%s",
                             job->attachment_kind,
                             job->message_id,
                             esp_err_to_name(err));
                }
                cap_im_tg_free_attachment_job(job);
                cap_im_tg_log_stack_watermark("tg_attachment");
            }
            continue;
        }

        if (s_tg.stop_requested) {
            break;
        }
    }

    s_tg.attachment_task = NULL;
    claw_task_delete(NULL);
}

static void cap_im_tg_queue_attachment(const char *chat_id,
                                       const char *sender_id,
                                       const char *message_id,
                                       const char *attachment_kind,
                                       const char *file_id,
                                       const char *original_filename,
                                       const char *mime,
                                       const char *caption,
                                       const char *content_type)
{
    cap_im_tg_attachment_job_t *job = NULL;

    if (!s_tg.enable_inbound_attachments || !s_tg.attachment_queue || !file_id || !file_id[0]) {
        return;
    }

    job = cap_im_tg_make_attachment_job(chat_id,
                                        sender_id,
                                        message_id,
                                        attachment_kind,
                                        file_id,
                                        original_filename,
                                        mime,
                                        caption,
                                        content_type);
    if (!job) {
        ESP_LOGW(TAG, "Telegram attachment queue alloc failed message=%s", message_id);
        return;
    }

    if (xQueueSend(s_tg.attachment_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG,
                 "Telegram attachment queue full message=%s kind=%s",
                 message_id,
                 attachment_kind);
        cap_im_tg_free_attachment_job(job);
        return;
    }
}

static void cap_im_tg_handle_update(cJSON *update_json)
{
    cJSON *update_id_json;
    cJSON *message_json;
    cJSON *chat_json;
    cJSON *chat_id_json;
    cJSON *from_json;
    cJSON *from_id_json;
    cJSON *message_id_json;
    cJSON *text_json;
    cJSON *document_json;
    cJSON *photo_json;
    const char *caption = NULL;
    char update_key[32];
    char chat_id[32];
    char sender_id[32];
    char message_id[32];
    int64_t update_id;

    if (!cJSON_IsObject(update_json)) {
        return;
    }

    update_id_json = cJSON_GetObjectItem(update_json, "update_id");
    message_json = cJSON_GetObjectItem(update_json, "message");
    if (!cJSON_IsNumber(update_id_json) || !cJSON_IsObject(message_json)) {
        return;
    }

    update_id = (int64_t)update_id_json->valuedouble;
    if (update_id >= s_tg.next_update_id) {
        s_tg.next_update_id = update_id + 1;
    }

    snprintf(update_key, sizeof(update_key), "%" PRId64, update_id);
    if (cap_im_tg_dedup_check_and_record(update_key)) {
        return;
    }

    chat_json = cJSON_GetObjectItem(message_json, "chat");
    from_json = cJSON_GetObjectItem(message_json, "from");
    message_id_json = cJSON_GetObjectItem(message_json, "message_id");
    if (!cJSON_IsObject(chat_json) || !cJSON_IsNumber(message_id_json)) {
        return;
    }

    chat_id_json = cJSON_GetObjectItem(chat_json, "id");
    from_id_json = cJSON_IsObject(from_json) ? cJSON_GetObjectItem(from_json, "id") : NULL;
    if (!cJSON_IsNumber(chat_id_json)) {
        return;
    }

    snprintf(chat_id, sizeof(chat_id), "%" PRId64, (int64_t)chat_id_json->valuedouble);
    if (cJSON_IsNumber(from_id_json)) {
        snprintf(sender_id, sizeof(sender_id), "%" PRId64, (int64_t)from_id_json->valuedouble);
    } else {
        sender_id[0] = '\0';
    }
    snprintf(message_id, sizeof(message_id), "%" PRId64, (int64_t)message_id_json->valuedouble);

    caption = cJSON_GetStringValue(cJSON_GetObjectItem(message_json, "caption"));
    photo_json = cap_im_tg_select_best_photo(message_json);
    if (s_tg.enable_inbound_attachments && photo_json) {
        const char *file_id = cJSON_GetStringValue(cJSON_GetObjectItem(photo_json, "file_id"));

        ESP_LOGI(TAG, "Telegram photo message=%s chat=%s", message_id, chat_id);

        if (file_id && file_id[0]) {
            cap_im_tg_queue_attachment(chat_id,
                                       sender_id,
                                       message_id,
                                       "photo",
                                       file_id,
                                       "photo.jpg",
                                       "image/jpeg",
                                       caption,
                                       "image");
        }
    }

    document_json = cJSON_GetObjectItem(message_json, "document");
    if (s_tg.enable_inbound_attachments && cJSON_IsObject(document_json)) {
        const char *file_id = cJSON_GetStringValue(cJSON_GetObjectItem(document_json, "file_id"));
        const char *file_name = cJSON_GetStringValue(cJSON_GetObjectItem(document_json, "file_name"));
        const char *mime = cJSON_GetStringValue(cJSON_GetObjectItem(document_json, "mime_type"));

        ESP_LOGI(TAG,
                 "Telegram document message=%s chat=%s filename=%s",
                 message_id,
                 chat_id,
                 file_name ? file_name : "");

        if (file_id && file_id[0]) {
            cap_im_tg_queue_attachment(chat_id,
                                       sender_id,
                                       message_id,
                                       "document",
                                       file_id,
                                       file_name,
                                       mime,
                                       caption,
                                       "file");
        }
    }

    text_json = cJSON_GetObjectItem(message_json, "text");
    if (cJSON_IsString(text_json) && text_json->valuestring && text_json->valuestring[0]) {
        if (cap_im_tg_publish_inbound_text(chat_id,
                                           sender_id,
                                           message_id,
                                           text_json->valuestring) == ESP_OK) {
            ESP_LOGI(TAG, "Telegram inbound %s: %.48s%s",
                     chat_id,
                     text_json->valuestring,
                     strlen(text_json->valuestring) > 48 ? "..." : "");
        } else {
            ESP_LOGW(TAG, "Failed to publish Telegram inbound message");
        }
    }
}

static esp_err_t cap_im_tg_poll_once(void)
{
    cJSON *body = NULL;
    char *body_json = NULL;
    char *resp = NULL;
    cJSON *root = NULL;
    cJSON *ok_json;
    cJSON *result_json;
    esp_err_t err;
    int i;

    body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(body, "timeout", CAP_IM_TG_POLL_TIMEOUT_S);
    if (s_tg.next_update_id > 0) {
        cJSON_AddNumberToObject(body, "offset", (double)s_tg.next_update_id);
    }

    body_json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_tg_api_call("getUpdates", body_json, &resp);
    free(body_json);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        return ESP_FAIL;
    }

    ok_json = cJSON_GetObjectItem(root, "ok");
    result_json = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsTrue(ok_json) || !cJSON_IsArray(result_json)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    for (i = 0; i < cJSON_GetArraySize(result_json); i++) {
        cap_im_tg_handle_update(cJSON_GetArrayItem(result_json, i));
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_im_tg_send_text_chunk(const char *chat_id, const char *message)
{
    cJSON *body = NULL;
    char *body_json = NULL;
    char *resp = NULL;
    esp_err_t err;

    body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "chat_id", chat_id);
    cJSON_AddStringToObject(body, "text", message);
    body_json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_tg_api_call("sendMessage", body_json, &resp);
    free(body_json);
    free(resp);
    return err;
}

static const char *cap_im_tg_basename(const char *path)
{
    const char *slash = NULL;

    if (!path || !path[0]) {
        return "";
    }

    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char *cap_im_tg_guess_mime_type(const char *path, bool is_image)
{
    const char *ext = NULL;

    if (!path) {
        return is_image ? "image/jpeg" : "application/octet-stream";
    }

    ext = strrchr(path, '.');
    if (!ext) {
        return is_image ? "image/jpeg" : "application/octet-stream";
    }

    if (strcasecmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(ext, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(ext, ".webp") == 0) {
        return "image/webp";
    }
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(ext, ".pdf") == 0) {
        return "application/pdf";
    }
    if (strcasecmp(ext, ".txt") == 0) {
        return "text/plain";
    }
    if (strcasecmp(ext, ".json") == 0) {
        return "application/json";
    }

    return is_image ? "image/jpeg" : "application/octet-stream";
}

static esp_err_t cap_im_tg_http_client_write_all(esp_http_client_handle_t client,
                                                 const char *data,
                                                 size_t len)
{
    size_t total = 0;

    if (!client || (!data && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    while (total < len) {
        int written = esp_http_client_write(client, data + total, (int)(len - total));

        if (written <= 0) {
            return ESP_FAIL;
        }

        total += (size_t)written;
    }

    return ESP_OK;
}

static esp_err_t cap_im_tg_stream_file_to_http_client(esp_http_client_handle_t client, FILE *file)
{
    char buf[1024];

    if (!client || !file) {
        return ESP_ERR_INVALID_ARG;
    }

    while (!feof(file)) {
        size_t nread = fread(buf, 1, sizeof(buf), file);

        if (nread > 0) {
            esp_err_t err = cap_im_tg_http_client_write_all(client, buf, nread);
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

static esp_err_t cap_im_tg_send_multipart_file(const char *method,
                                               const char *field_name,
                                               const char *chat_id,
                                               const char *path,
                                               const char *caption,
                                               bool is_image)
{
    struct stat st;
    FILE *file = NULL;
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    cap_im_tg_http_resp_t resp = {0};
    char *url = NULL;
    const char *mime = NULL;
    const char *file_name = NULL;
    char content_type[128];
    char part_chat[256];
    char part_caption[1024];
    char part_file[512];
    char closing[64];
    size_t content_length;
    int needed;
    int status;
    int part_chat_len;
    int part_caption_len = 0;
    int part_file_len;
    int closing_len;
    esp_err_t err = ESP_FAIL;

    if (!method || !field_name || !chat_id || !chat_id[0] || !path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tg.bot_token[0] == '\0') {
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

    needed = snprintf(NULL, 0, "%s/bot%s/%s", CAP_IM_TG_API_BASE, s_tg.bot_token, method);
    if (needed < 0) {
        return ESP_FAIL;
    }

    url = calloc(1, (size_t)needed + 1);
    resp.buf = calloc(1, CAP_IM_TG_HTTP_RESP_INIT);
    if (!url || !resp.buf) {
        free(url);
        free(resp.buf);
        return ESP_ERR_NO_MEM;
    }
    resp.cap = CAP_IM_TG_HTTP_RESP_INIT;
    snprintf(url, (size_t)needed + 1, "%s/bot%s/%s", CAP_IM_TG_API_BASE, s_tg.bot_token, method);

    mime = cap_im_tg_guess_mime_type(path, is_image);
    file_name = cap_im_tg_basename(path);

    part_chat_len = snprintf(part_chat, sizeof(part_chat),
                             "--" CAP_IM_TG_MULTIPART_BOUNDARY "\r\n"
                             "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
                             "%s\r\n",
                             chat_id);
    if (part_chat_len <= 0 || part_chat_len >= (int)sizeof(part_chat)) {
        free(url);
        free(resp.buf);
        return ESP_ERR_INVALID_SIZE;
    }

    if (caption && caption[0]) {
        part_caption_len = snprintf(part_caption, sizeof(part_caption),
                                    "--" CAP_IM_TG_MULTIPART_BOUNDARY "\r\n"
                                    "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
                                    "%s\r\n",
                                    caption);
        if (part_caption_len <= 0 || part_caption_len >= (int)sizeof(part_caption)) {
            free(url);
            free(resp.buf);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    part_file_len = snprintf(part_file, sizeof(part_file),
                             "--" CAP_IM_TG_MULTIPART_BOUNDARY "\r\n"
                             "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                             "Content-Type: %s\r\n\r\n",
                             field_name,
                             file_name,
                             mime);
    if (part_file_len <= 0 || part_file_len >= (int)sizeof(part_file)) {
        free(url);
        free(resp.buf);
        return ESP_ERR_INVALID_SIZE;
    }

    closing_len = snprintf(closing, sizeof(closing),
                           "\r\n--" CAP_IM_TG_MULTIPART_BOUNDARY "--\r\n");
    if (closing_len <= 0 || closing_len >= (int)sizeof(closing)) {
        free(url);
        free(resp.buf);
        return ESP_ERR_INVALID_SIZE;
    }

    content_length = (size_t)part_chat_len + (size_t)part_caption_len +
                     (size_t)part_file_len + (size_t)st.st_size + (size_t)closing_len;

    file = fopen(path, "rb");
    if (!file) {
        free(url);
        free(resp.buf);
        return ESP_FAIL;
    }

    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.event_handler = cap_im_tg_http_event_handler;
    config.user_data = &resp;
    config.timeout_ms = (CAP_IM_TG_POLL_TIMEOUT_S + 5) * 1000;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (!client) {
        fclose(file);
        free(url);
        free(resp.buf);
        return ESP_FAIL;
    }

    snprintf(content_type,
             sizeof(content_type),
             "multipart/form-data; boundary=" CAP_IM_TG_MULTIPART_BOUNDARY);
    esp_http_client_set_header(client, "Content-Type", content_type);

    err = esp_http_client_open(client, (int)content_length);
    if (err == ESP_OK) {
        err = cap_im_tg_http_client_write_all(client, part_chat, (size_t)part_chat_len);
    }
    if (err == ESP_OK && part_caption_len > 0) {
        err = cap_im_tg_http_client_write_all(client, part_caption, (size_t)part_caption_len);
    }
    if (err == ESP_OK) {
        err = cap_im_tg_http_client_write_all(client, part_file, (size_t)part_file_len);
    }
    if (err == ESP_OK) {
        err = cap_im_tg_stream_file_to_http_client(client, file);
    }
    if (err == ESP_OK) {
        err = cap_im_tg_http_client_write_all(client, closing, (size_t)closing_len);
    }
    if (err == ESP_OK) {
        if (esp_http_client_fetch_headers(client) < 0) {
            err = ESP_FAIL;
        }
    }

    status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    fclose(file);
    free(url);

    if (err != ESP_OK) {
        free(resp.buf);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Telegram %s failed: http=%d body=%s", method, status, resp.buf ? resp.buf : "");
        free(resp.buf);
        return ESP_FAIL;
    }

    free(resp.buf);
    return ESP_OK;
}

static esp_err_t cap_im_tg_send_media(const char *chat_id,
                                      const char *path,
                                      const char *caption,
                                      bool is_image)
{
    esp_err_t err;
    const char *method = is_image ? "sendPhoto" : "sendDocument";
    const char *field_name = is_image ? "photo" : "document";

    err = cap_im_tg_send_multipart_file(method, field_name, chat_id, path, caption, is_image);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Telegram %s send failed chat=%s path=%s err=%s",
                 is_image ? "image" : "file",
                 chat_id ? chat_id : "",
                 path ? path : "",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "Telegram %s send success to %s: %s",
             is_image ? "image" : "file",
             chat_id,
             path);
    return ESP_OK;
}

static void cap_im_tg_poll_task(void *arg)
{
    (void)arg;

    while (!s_tg.stop_requested) {
        if (cap_im_tg_poll_once() != ESP_OK) {
            if (s_tg.stop_requested) {
                break;
            }

            ESP_LOGW(TAG, "Telegram polling failed, retrying");
            vTaskDelay(pdMS_TO_TICKS(CAP_IM_TG_RETRY_DELAY_MS));
        }
    }

    s_tg.poll_task = NULL;
    claw_task_delete(NULL);
}

static esp_err_t cap_im_tg_gateway_init(void)
{
    if (s_tg.bot_token[0] == '\0') {
        ESP_LOGW(TAG, "Telegram bot token not configured");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Telegram configured");
    return ESP_OK;
}

static esp_err_t cap_im_tg_gateway_start(void)
{
    BaseType_t ok;

    if (s_tg.bot_token[0] == '\0') {
        ESP_LOGW(TAG, "Telegram not configured, skipping start");
        return ESP_OK;
    }
    if (s_tg.poll_task) {
        return ESP_OK;
    }

    s_tg.stop_requested = false;
    if (!s_tg.attachment_queue) {
        s_tg.attachment_queue = xQueueCreate(CAP_IM_TG_ATTACHMENT_QUEUE_LEN,
                                             sizeof(cap_im_tg_attachment_job_t *));
        if (!s_tg.attachment_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    ok = claw_task_create(&(claw_task_config_t){
                              .name = "tg_attach",
                              .stack_size = 8192,
                              .priority = 5,
                              .core_id = tskNO_AFFINITY,
                              .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                          },
                          cap_im_tg_attachment_task,
                          NULL,
                          &s_tg.attachment_task);
    if (ok != pdPASS) {
        vQueueDelete(s_tg.attachment_queue);
        s_tg.attachment_queue = NULL;
        s_tg.attachment_task = NULL;
        return ESP_FAIL;
    }
    ok = claw_task_create(&(claw_task_config_t){
                              .name = "tg_poll",
                              .stack_size = 6144,
                              .priority = 5,
                              .core_id = tskNO_AFFINITY,
                              .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                          },
                          cap_im_tg_poll_task,
                          NULL,
                          &s_tg.poll_task);
    if (ok != pdPASS) {
        s_tg.stop_requested = true;
        s_tg.poll_task = NULL;
        while (s_tg.attachment_task) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vQueueDelete(s_tg.attachment_queue);
        s_tg.attachment_queue = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_im_tg_gateway_stop(void)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    if (!s_tg.poll_task) {
        return ESP_OK;
    }

    s_tg.stop_requested = true;
    while (s_tg.poll_task && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_tg.poll_task) {
        return ESP_ERR_TIMEOUT;
    }

    while (s_tg.attachment_task && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_tg.attachment_task) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_tg.attachment_queue) {
        cap_im_tg_attachment_job_t *job = NULL;

        while (xQueueReceive(s_tg.attachment_queue, &job, 0) == pdTRUE) {
            cap_im_tg_free_attachment_job(job);
        }
        vQueueDelete(s_tg.attachment_queue);
        s_tg.attachment_queue = NULL;
    }

    return ESP_OK;
}

static esp_err_t cap_im_tg_send_message_execute(const char *input_json,
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
        cJSON_Delete(root);
        snprintf(output,
                 output_size,
                 "Error: chat_id and message are required (chat_id may come from ctx)");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_tg_send_text(chat_id, message);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: %s", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "reply already sent to user");
    return ESP_OK;
}

static esp_err_t cap_im_tg_send_media_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size,
                                              bool is_image)
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

    err = cap_im_tg_send_media(chat_id, path, caption, is_image);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: %s", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "reply already sent to user");
    return ESP_OK;
}

static esp_err_t cap_im_tg_send_image_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    return cap_im_tg_send_media_execute(input_json, ctx, output, output_size, true);
}

static esp_err_t cap_im_tg_send_file_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    return cap_im_tg_send_media_execute(input_json, ctx, output, output_size, false);
}

static const claw_cap_descriptor_t s_tg_descriptors[] = {
    {
        .id = "tg_gateway",
        .name = "tg_gateway",
        .family = "im",
        .description = "Telegram bot polling gateway event source.",
        .kind = CLAW_CAP_KIND_EVENT_SOURCE,
        .cap_flags = CLAW_CAP_FLAG_EMITS_EVENTS |
        CLAW_CAP_FLAG_SUPPORTS_LIFECYCLE,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .init = cap_im_tg_gateway_init,
        .start = cap_im_tg_gateway_start,
        .stop = cap_im_tg_gateway_stop,
    },
    {
        .id = "tg_send_message",
        .name = "tg_send_message",
        .family = "im",
        .description = "Send a text message to an explicit Telegram chat_id.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"}},\"required\":[\"chat_id\",\"message\"]}",
        .execute = cap_im_tg_send_message_execute,
    },
    {
        .id = "tg_send_image",
        .name = "tg_send_image",
        .family = "im",
        .description = "Send an image file from a local path to a Telegram chat.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"caption\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        .execute = cap_im_tg_send_image_execute,
    },
    {
        .id = "tg_send_file",
        .name = "tg_send_file",
        .family = "im",
        .description = "Send a file from a local path to a Telegram chat.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"caption\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        .execute = cap_im_tg_send_file_execute,
    },
};

static const claw_cap_group_t s_tg_group = {
    .group_id = "cap_im_tg",
    .descriptors = s_tg_descriptors,
    .descriptor_count = sizeof(s_tg_descriptors) / sizeof(s_tg_descriptors[0]),
};

esp_err_t cap_im_tg_register_group(void)
{
    cap_im_tg_init_defaults();

    if (claw_cap_group_exists(s_tg_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_tg_group);
}

esp_err_t cap_im_tg_set_token(const char *bot_token)
{
    if (!s_tg_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!bot_token) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_tg.bot_token, bot_token, sizeof(s_tg.bot_token));
    s_tg.next_update_id = 0;
    memset(s_tg.seen_update_keys, 0, sizeof(s_tg.seen_update_keys));
    s_tg.seen_update_idx = 0;
    return ESP_OK;
}

esp_err_t cap_im_tg_set_attachment_config(
    const cap_im_tg_attachment_config_t *config)
{
    if (!s_tg_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    s_tg.enable_inbound_attachments = config->enable_inbound_attachments;
    s_tg.max_inbound_file_bytes = config->max_inbound_file_bytes;
    if (config->storage_root_dir) {
        strlcpy(s_tg.attachment_root_dir,
                config->storage_root_dir,
                sizeof(s_tg.attachment_root_dir));
    } else {
        s_tg.attachment_root_dir[0] = '\0';
    }

    return ESP_OK;
}

esp_err_t cap_im_tg_start(void)
{
    if (!s_tg_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_tg.bot_token[0] == '\0') {
        ESP_LOGE(TAG, "Telegram bot token is not configured");
        return ESP_ERR_INVALID_STATE;
    }

    return cap_im_tg_gateway_start();
}

esp_err_t cap_im_tg_stop(void)
{
    return cap_im_tg_gateway_stop();
}

esp_err_t cap_im_tg_send_text(const char *chat_id, const char *text)
{
    if (!s_tg_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t text_len;
    size_t offset = 0;
    esp_err_t last_err = ESP_OK;

    if (!chat_id || !text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tg.bot_token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    text_len = strlen(text);
    while (offset < text_len) {
        size_t chunk_len = text_len - offset;
        char *chunk = NULL;
        esp_err_t err;

        if (chunk_len > CAP_IM_TG_MAX_MSG_LEN) {
            chunk_len = CAP_IM_TG_MAX_MSG_LEN;
        }

        chunk = calloc(1, chunk_len + 1);
        if (!chunk) {
            return ESP_ERR_NO_MEM;
        }

        memcpy(chunk, text + offset, chunk_len);
        err = cap_im_tg_send_text_chunk(chat_id, chunk);
        free(chunk);
        if (err != ESP_OK) {
            last_err = err;
        }

        offset += chunk_len;
    }

    return last_err;
}

esp_err_t cap_im_tg_send_image(const char *chat_id, const char *path, const char *caption)
{
    if (!s_tg_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return cap_im_tg_send_media(chat_id, path, caption, true);
}

esp_err_t cap_im_tg_send_file(const char *chat_id, const char *path, const char *caption)
{
    if (!s_tg_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return cap_im_tg_send_media(chat_id, path, caption, false);
}
