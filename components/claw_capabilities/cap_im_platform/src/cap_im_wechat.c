/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_wechat.h"
#include "claw_utils_string.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cap_im_attachment.h"
#include "cJSON.h"
#include "claw_cap.h"
#include "claw_task.h"
#include "claw_event_publisher.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "aes/esp_aes.h"
#include "mbedtls/base64.h"
#include "esp_rom_md5.h"

static const char *TAG = "cap_im_wechat";

#define CAP_IM_WECHAT_HTTP_RESP_INIT 2048
#define CAP_IM_WECHAT_MAX_MSG_LEN 4000
#define CAP_IM_WECHAT_POLL_TIMEOUT_MS 35000
#define CAP_IM_WECHAT_RETRY_DELAY_MS 2000
#define CAP_IM_WECHAT_DEDUP_CACHE_SIZE 64
#define CAP_IM_WECHAT_CONTEXT_CACHE_SIZE 32
#define CAP_IM_WECHAT_STAGE_CACHE_SIZE 32
#define CAP_IM_WECHAT_STAGE_LIMIT 5
#define CAP_IM_WECHAT_PATH_BUF_SIZE 256
#define CAP_IM_WECHAT_NAME_BUF_SIZE 96
#define CAP_IM_WECHAT_URL_BUF_SIZE 384
#define CAP_IM_WECHAT_TOKEN_SIZE 256
#define CAP_IM_WECHAT_BASE_URL_SIZE 160
#define CAP_IM_WECHAT_ACCOUNT_ID_SIZE 64
#define CAP_IM_WECHAT_APP_ID_SIZE 64
#define CAP_IM_WECHAT_CLIENT_VERSION_SIZE 32
#define CAP_IM_WECHAT_ROUTE_TAG_SIZE 64
#define CAP_IM_WECHAT_CLIENT_ID_SIZE 48
#define CAP_IM_WECHAT_BODY_BUF_SIZE 1536
#define CAP_IM_WECHAT_DEFAULT_BASE_URL "https://ilinkai.weixin.qq.com"
#define CAP_IM_WECHAT_DEFAULT_CDN_BASE_URL "https://novac2c.cdn.weixin.qq.com/c2c"
#define CAP_IM_WECHAT_DEFAULT_APP_ID "bot"
#define CAP_IM_WECHAT_DEFAULT_CLIENT_VERSION "131329"
#define CAP_IM_WECHAT_QR_POLL_TIMEOUT_MS 35000
#define CAP_IM_WECHAT_QR_START_TIMEOUT_MS 5000
#define CAP_IM_WECHAT_QR_TTL_MS (5 * 60 * 1000)
#define CAP_IM_WECHAT_QR_MAX_REFRESH 3

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    char *encrypted_param;
} cap_im_wechat_http_resp_t;

typedef struct {
    char chat_id[72];
    char context_token[160];
} cap_im_wechat_context_entry_t;

typedef struct {
    char chat_id[72];
    uint8_t consecutive_stage_count;
} cap_im_wechat_stage_entry_t;

typedef enum {
    CAP_IM_WECHAT_STAGE_SEND_ORIGINAL = 0,
    CAP_IM_WECHAT_STAGE_SEND_LIMIT_NOTICE = 1,
    CAP_IM_WECHAT_STAGE_SKIP = 2,
} cap_im_wechat_stage_send_mode_t;

typedef struct {
    bool active;
    bool completed;
    bool persisted;
    bool stop_requested;
    char session_key[64];
    char account_id[64];
    char status[32];
    char message[160];
    char qrcode[96];
    char qr_data_url[256];
    char bot_token[CAP_IM_WECHAT_TOKEN_SIZE];
    char ilink_bot_id[64];
    char ilink_user_id[96];
    char base_url[CAP_IM_WECHAT_BASE_URL_SIZE];
    char current_api_base_url[CAP_IM_WECHAT_BASE_URL_SIZE];
    int64_t started_at_ms;
    uint8_t refresh_count;
} cap_im_wechat_qr_state_t;

typedef struct {
    char token[CAP_IM_WECHAT_TOKEN_SIZE];
    char base_url[CAP_IM_WECHAT_BASE_URL_SIZE];
    char cdn_base_url[CAP_IM_WECHAT_BASE_URL_SIZE];
    char account_id[CAP_IM_WECHAT_ACCOUNT_ID_SIZE];
    char app_id[CAP_IM_WECHAT_APP_ID_SIZE];
    char client_version[CAP_IM_WECHAT_CLIENT_VERSION_SIZE];
    char route_tag[CAP_IM_WECHAT_ROUTE_TAG_SIZE];
    char attachment_root_dir[CAP_IM_WECHAT_PATH_BUF_SIZE];
    size_t max_inbound_file_bytes;
    bool enable_inbound_attachments;
    bool configured;
    bool stop_requested;
    int poll_timeout_ms;
    char *sync_buf;
    TaskHandle_t poll_task;
    TaskHandle_t qr_task;
    SemaphoreHandle_t lock;
    uint64_t seen_msg_keys[CAP_IM_WECHAT_DEDUP_CACHE_SIZE];
    size_t seen_msg_idx;
    cap_im_wechat_context_entry_t context_cache[CAP_IM_WECHAT_CONTEXT_CACHE_SIZE];
    size_t context_idx;
    cap_im_wechat_stage_entry_t stage_cache[CAP_IM_WECHAT_STAGE_CACHE_SIZE];
    size_t stage_idx;
    cap_im_wechat_qr_state_t qr;
} cap_im_wechat_state_t;

static void cap_im_wechat_qr_reset_locked(void);
static esp_err_t cap_im_wechat_qr_fetch_code_locked(void);
static esp_err_t cap_im_wechat_qr_poll_once_locked(void);
static void cap_im_wechat_qr_task(void *arg);

static cap_im_wechat_state_t *s_wechat_state = NULL;
#define s_wechat (*s_wechat_state)

static void cap_im_wechat_init_defaults(cap_im_wechat_state_t *state)
{
    if (!state) {
        return;
    }

    strlcpy(state->base_url, CAP_IM_WECHAT_DEFAULT_BASE_URL, sizeof(state->base_url));
    strlcpy(state->cdn_base_url,
            CAP_IM_WECHAT_DEFAULT_CDN_BASE_URL,
            sizeof(state->cdn_base_url));
    strlcpy(state->account_id, "default", sizeof(state->account_id));
    strlcpy(state->app_id, CAP_IM_WECHAT_DEFAULT_APP_ID, sizeof(state->app_id));
    strlcpy(state->client_version,
            CAP_IM_WECHAT_DEFAULT_CLIENT_VERSION,
            sizeof(state->client_version));
    state->max_inbound_file_bytes = 2 * 1024 * 1024;
    state->poll_timeout_ms = CAP_IM_WECHAT_POLL_TIMEOUT_MS;
}

static esp_err_t cap_im_wechat_ensure_state(void)
{
    if (s_wechat_state) {
        return ESP_OK;
    }

    s_wechat_state = calloc(1, sizeof(*s_wechat_state));
    if (!s_wechat_state) {
        return ESP_ERR_NO_MEM;
    }

    cap_im_wechat_init_defaults(s_wechat_state);
    return ESP_OK;
}

static void cap_im_wechat_free_state(void)
{
    if (!s_wechat_state) {
        return;
    }

    free(s_wechat.sync_buf);
    s_wechat.sync_buf = NULL;
    if (s_wechat.lock) {
        vSemaphoreDelete(s_wechat.lock);
        s_wechat.lock = NULL;
    }

    free(s_wechat_state);
    s_wechat_state = NULL;
}

static int64_t cap_im_wechat_now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static esp_err_t cap_im_wechat_lock(void)
{
    esp_err_t err = cap_im_wechat_ensure_state();

    if (err != ESP_OK) {
        return err;
    }

    if (!s_wechat.lock) {
        s_wechat.lock = xSemaphoreCreateMutex();
        if (!s_wechat.lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    return xSemaphoreTake(s_wechat.lock, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void cap_im_wechat_unlock(void)
{
    if (s_wechat.lock) {
        xSemaphoreGive(s_wechat.lock);
    }
}

static void cap_im_wechat_random_session_key(char *buf, size_t buf_size)
{
    uint32_t a = esp_random();
    uint32_t b = esp_random();

    snprintf(buf, buf_size, "wxqr-%08" PRIx32 "%08" PRIx32, a, b);
}

static uint64_t cap_im_wechat_fnv1a64(const char *text)
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

static bool cap_im_wechat_dedup_check_and_record(const char *message_id)
{
    uint64_t key;
    size_t i;

    if (!message_id || !message_id[0]) {
        return false;
    }

    key = cap_im_wechat_fnv1a64(message_id);
    for (i = 0; i < CAP_IM_WECHAT_DEDUP_CACHE_SIZE; i++) {
        if (s_wechat.seen_msg_keys[i] == key) {
            return true;
        }
    }

    s_wechat.seen_msg_keys[s_wechat.seen_msg_idx] = key;
    s_wechat.seen_msg_idx =
        (s_wechat.seen_msg_idx + 1) % CAP_IM_WECHAT_DEDUP_CACHE_SIZE;
    return false;
}

static const char *cap_im_wechat_string_value(cJSON *item)
{
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static cJSON *cap_im_wechat_require_object(cJSON *root, const char *key)
{
    cJSON *item = NULL;

    if (!root || !key) {
        return NULL;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsObject(item) ? item : NULL;
}

static cJSON *cap_im_wechat_require_array(cJSON *root, const char *key)
{
    cJSON *item = NULL;

    if (!root || !key) {
        return NULL;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsArray(item) ? item : NULL;
}

static int cap_im_wechat_int_value(cJSON *item, int fallback)
{
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return fallback;
}

static int64_t cap_im_wechat_int64_value(cJSON *item, int64_t fallback)
{
    if (cJSON_IsNumber(item)) {
        return (int64_t)item->valuedouble;
    }
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        return strtoll(item->valuestring, NULL, 10);
    }
    return fallback;
}

static void cap_im_wechat_context_remember(const char *chat_id, const char *context_token)
{
    size_t i;
    size_t slot;

    if (!chat_id || !chat_id[0] || !context_token || !context_token[0]) {
        return;
    }

    for (i = 0; i < CAP_IM_WECHAT_CONTEXT_CACHE_SIZE; i++) {
        if (strcmp(s_wechat.context_cache[i].chat_id, chat_id) == 0) {
            strlcpy(s_wechat.context_cache[i].context_token,
                    context_token,
                    sizeof(s_wechat.context_cache[i].context_token));
            return;
        }
    }

    slot = s_wechat.context_idx;
    s_wechat.context_idx =
        (s_wechat.context_idx + 1) % CAP_IM_WECHAT_CONTEXT_CACHE_SIZE;
    strlcpy(s_wechat.context_cache[slot].chat_id,
            chat_id,
            sizeof(s_wechat.context_cache[slot].chat_id));
    strlcpy(s_wechat.context_cache[slot].context_token,
            context_token,
            sizeof(s_wechat.context_cache[slot].context_token));
}

static const char *cap_im_wechat_context_lookup(const char *chat_id)
{
    size_t i;

    if (!chat_id || !chat_id[0]) {
        return NULL;
    }

    for (i = 0; i < CAP_IM_WECHAT_CONTEXT_CACHE_SIZE; i++) {
        if (strcmp(s_wechat.context_cache[i].chat_id, chat_id) == 0 &&
                s_wechat.context_cache[i].context_token[0]) {
            return s_wechat.context_cache[i].context_token;
        }
    }

    return NULL;
}

static cap_im_wechat_stage_entry_t *cap_im_wechat_stage_entry_find_or_create(const char *chat_id)
{
    size_t slot;

    if (!chat_id || !chat_id[0]) {
        return NULL;
    }

    for (size_t i = 0; i < CAP_IM_WECHAT_STAGE_CACHE_SIZE; i++) {
        if (strcmp(s_wechat.stage_cache[i].chat_id, chat_id) == 0) {
            return &s_wechat.stage_cache[i];
        }
    }

    slot = s_wechat.stage_idx;
    s_wechat.stage_idx = (s_wechat.stage_idx + 1) % CAP_IM_WECHAT_STAGE_CACHE_SIZE;
    memset(&s_wechat.stage_cache[slot], 0, sizeof(s_wechat.stage_cache[slot]));
    strlcpy(s_wechat.stage_cache[slot].chat_id,
            chat_id,
            sizeof(s_wechat.stage_cache[slot].chat_id));
    return &s_wechat.stage_cache[slot];
}

static cap_im_wechat_stage_send_mode_t cap_im_wechat_stage_send_mode_for_event(const char *chat_id,
                                                                               const char *event_type)
{
    cap_im_wechat_stage_entry_t *entry = NULL;

    if (!chat_id || !chat_id[0]) {
        return CAP_IM_WECHAT_STAGE_SEND_ORIGINAL;
    }

    if (cap_im_wechat_lock() != ESP_OK) {
        ESP_LOGW(TAG, "stage limiter lock failed for chat=%s; sending message", chat_id);
        return CAP_IM_WECHAT_STAGE_SEND_ORIGINAL;
    }

    entry = cap_im_wechat_stage_entry_find_or_create(chat_id);
    if (!entry) {
        cap_im_wechat_unlock();
        return CAP_IM_WECHAT_STAGE_SEND_ORIGINAL;
    }

    if (event_type && strcmp(event_type, "agent_stage") == 0) {
        if (entry->consecutive_stage_count < UINT8_MAX) {
            entry->consecutive_stage_count++;
        }
        if (entry->consecutive_stage_count <= CAP_IM_WECHAT_STAGE_LIMIT) {
            cap_im_wechat_unlock();
            return CAP_IM_WECHAT_STAGE_SEND_ORIGINAL;
        }
        if (entry->consecutive_stage_count == CAP_IM_WECHAT_STAGE_LIMIT + 1) {
            cap_im_wechat_unlock();
            return CAP_IM_WECHAT_STAGE_SEND_LIMIT_NOTICE;
        }
        cap_im_wechat_unlock();
        return CAP_IM_WECHAT_STAGE_SKIP;
    } else {
        entry->consecutive_stage_count = 0;
    }

    cap_im_wechat_unlock();
    return CAP_IM_WECHAT_STAGE_SEND_ORIGINAL;
}

static esp_err_t cap_im_wechat_http_event_handler(esp_http_client_event_t *event)
{
    cap_im_wechat_http_resp_t *resp =
        (cap_im_wechat_http_resp_t *)event->user_data;
    char *next = NULL;
    size_t needed;

    if (!resp) {
        return ESP_OK;
    }

    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
            event->header_value &&
            strcasecmp(event->header_key, "x-encrypted-param") == 0) {
        char *copy = strdup(event->header_value);

        if (!copy) {
            return ESP_ERR_NO_MEM;
        }
        free(resp->encrypted_param);
        resp->encrypted_param = copy;
        return ESP_OK;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data || event->data_len <= 0) {
        return ESP_OK;
    }

    needed = resp->len + (size_t)event->data_len + 1;
    if (needed > resp->cap) {
        size_t new_cap = resp->cap ? resp->cap : CAP_IM_WECHAT_HTTP_RESP_INIT;

        while (new_cap < needed) {
            new_cap *= 2;
        }

        next = realloc(resp->buf, new_cap);
        if (!next) {
            return ESP_ERR_NO_MEM;
        }

        resp->buf = next;
        resp->cap = new_cap;
    }

    memcpy(resp->buf + resp->len, event->data, (size_t)event->data_len);
    resp->len += (size_t)event->data_len;
    resp->buf[resp->len] = '\0';
    return ESP_OK;
}

static esp_err_t cap_im_wechat_resp_prepare(cap_im_wechat_http_resp_t *response)
{
    if (!response) {
        return ESP_OK;
    }

    if (!response->buf) {
        response->buf = calloc(1, CAP_IM_WECHAT_HTTP_RESP_INIT);
        if (!response->buf) {
            return ESP_ERR_NO_MEM;
        }
        response->cap = CAP_IM_WECHAT_HTTP_RESP_INIT;
    }

    response->len = 0;
    response->buf[0] = '\0';
    free(response->encrypted_param);
    response->encrypted_param = NULL;
    return ESP_OK;
}

static void cap_im_wechat_resp_cleanup(cap_im_wechat_http_resp_t *response)
{
    if (!response) {
        return;
    }

    free(response->buf);
    response->buf = NULL;
    response->len = 0;
    response->cap = 0;
    free(response->encrypted_param);
    response->encrypted_param = NULL;
}

static esp_err_t cap_im_wechat_build_x_wechat_uin(char *buf, size_t buf_size)
{
    uint32_t value = esp_random();
    char decimal[16];
    unsigned char encoded[32];
    size_t out_len = 0;
    int ret;

    if (!buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(decimal, sizeof(decimal), "%" PRIu32, value);
    ret = mbedtls_base64_encode(encoded,
                                sizeof(encoded),
                                &out_len,
                                (const unsigned char *)decimal,
                                strlen(decimal));
    if (ret != 0 || out_len + 1 > buf_size) {
        return ESP_FAIL;
    }

    memcpy(buf, encoded, out_len);
    buf[out_len] = '\0';
    return ESP_OK;
}

static esp_err_t cap_im_wechat_http_request(const char *url,
                                            esp_http_client_method_t method,
                                            const char *content_type,
                                            const void *body,
                                            size_t body_len,
                                            int timeout_ms,
                                            bool use_common_headers,
                                            bool use_auth_headers,
                                            cap_im_wechat_http_resp_t *response)
{
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status;

    if (!url || !content_type) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(cap_im_wechat_resp_prepare(response), TAG, "prepare response failed");

    config.url = url;
    config.method = method;
    config.timeout_ms = timeout_ms;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = cap_im_wechat_http_event_handler;
    config.user_data = response;
    config.buffer_size = 1024;
    config.buffer_size_tx = 2048;
#ifdef CONFIG_HTTP_REUSE_ENABLE
    config.keep_alive_enable = true;
#endif

    client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    if (content_type && content_type[0]) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (use_common_headers) {
        char x_wechat_uin[64];

        if (s_wechat.app_id[0]) {
            esp_http_client_set_header(client, "iLink-App-Id", s_wechat.app_id);
        }
        if (s_wechat.client_version[0]) {
            esp_http_client_set_header(client, "iLink-App-ClientVersion", s_wechat.client_version);
        }
        if (s_wechat.route_tag[0]) {
            esp_http_client_set_header(client, "SKRouteTag", s_wechat.route_tag);
        }
        err = cap_im_wechat_build_x_wechat_uin(x_wechat_uin, sizeof(x_wechat_uin));
        if (err != ESP_OK) {
            goto cleanup;
        }
        esp_http_client_set_header(client, "X-WECHAT-UIN", x_wechat_uin);
    }
    if (use_auth_headers) {
        esp_http_client_set_header(client, "AuthorizationType", "ilink_bot_token");
        if (s_wechat.token[0]) {
            char auth_header[CAP_IM_WECHAT_TOKEN_SIZE + 8];

            snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_wechat.token);
            esp_http_client_set_header(client, "Authorization", auth_header);
        }
    }
    if (body && body_len > 0) {
        esp_http_client_set_post_field(client, body, (int)body_len);
    }

    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        goto cleanup;
    }

    status = esp_http_client_get_status_code(client);
    err = ESP_OK;
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "WeChat HTTP %s failed: status=%d body=%s", url, status,
                 response && response->buf ? response->buf : "");
        err = ESP_FAIL;
    }

cleanup:
    if (client) {
        esp_http_client_cleanup(client);
    }
    return err;
}

static esp_err_t cap_im_wechat_api_post(const char *endpoint,
                                        cJSON *root,
                                        int timeout_ms,
                                        cap_im_wechat_http_resp_t *response)
{
    char url[CAP_IM_WECHAT_URL_BUF_SIZE];
    char *body_json = NULL;
    esp_err_t err;

    if (!endpoint || !root || !s_wechat.configured) {
        return ESP_ERR_INVALID_STATE;
    }

    if (snprintf(url, sizeof(url), "%s/%s", s_wechat.base_url, endpoint) >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    body_json = cJSON_PrintUnformatted(root);
    if (!body_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_wechat_http_request(url,
                                     HTTP_METHOD_POST,
                                     "application/json",
                                     body_json,
                                     strlen(body_json),
                                     timeout_ms,
                                     true,
                                     true,
                                     response);
    free(body_json);
    return err;
}

static esp_err_t cap_im_wechat_http_get_json(const char *base_url,
                                             const char *endpoint,
                                             int timeout_ms,
                                             cap_im_wechat_http_resp_t *response)
{
    char url[CAP_IM_WECHAT_URL_BUF_SIZE];

    if (!base_url || !endpoint) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(url, sizeof(url), "%s/%s", base_url, endpoint) >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return cap_im_wechat_http_request(url,
                                      HTTP_METHOD_GET,
                                      "application/json",
                                      NULL,
                                      0,
                                      timeout_ms,
                                      true,
                                      false,
                                      response);
}

static esp_err_t cap_im_wechat_add_base_info(cJSON *root)
{
    cJSON *base_info = NULL;

    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    base_info = cJSON_AddObjectToObject(root, "base_info");
    if (!base_info) {
        return ESP_ERR_NO_MEM;
    }

    if (!cJSON_AddStringToObject(base_info, "channel_version", "esp-claw-wechat")) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t cap_im_wechat_publish_inbound_text(const char *chat_id,
                                                    const char *sender_id,
                                                    const char *message_id,
                                                    const char *content)
{
    if (!content || !content[0]) {
        return ESP_OK;
    }

    return claw_event_router_publish_message("wechat_gateway",
                                             "wechat",
                                             chat_id,
                                             content,
                                             sender_id,
                                             message_id);
}

static esp_err_t cap_im_wechat_publish_attachment_event(const char *chat_id,
                                                        const char *sender_id,
                                                        const char *message_id,
                                                        const char *content_type,
                                                        const char *payload_json)
{
    claw_event_t event = {0};

    if (!chat_id || !message_id || !content_type || !payload_json) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(event.source_cap, "wechat_gateway", sizeof(event.source_cap));
    strlcpy(event.event_type, "attachment_saved", sizeof(event.event_type));
    strlcpy(event.source_channel, "wechat", sizeof(event.source_channel));
    strlcpy(event.chat_id, chat_id, sizeof(event.chat_id));
    if (sender_id && sender_id[0]) {
        strlcpy(event.sender_id, sender_id, sizeof(event.sender_id));
    }
    strlcpy(event.message_id, message_id, sizeof(event.message_id));
    strlcpy(event.content_type, content_type, sizeof(event.content_type));
    event.timestamp_ms = cap_im_wechat_now_ms();
    event.session_policy = CLAW_SESSION_POLICY_CHAT;
    snprintf(event.event_id, sizeof(event.event_id), "wechat-attach-%" PRId64, event.timestamp_ms);
    event.text = "";
    event.payload_json = (char *)payload_json;
    return claw_event_router_publish(&event);
}

static esp_err_t cap_im_wechat_download_buffer(const char *url,
                                               size_t max_bytes,
                                               unsigned char **out_buf,
                                               size_t *out_len)
{
    cap_im_wechat_http_resp_t response = {0};
    esp_err_t err;

    if (!url || !out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_wechat_http_request(url,
                                     HTTP_METHOD_GET,
                                     "",
                                     NULL,
                                     0,
                                     30000,
                                     false,
                                     false,
                                     &response);
    if (err != ESP_OK) {
        cap_im_wechat_resp_cleanup(&response);
        return err;
    }

    if (response.len > max_bytes) {
        cap_im_wechat_resp_cleanup(&response);
        return ESP_ERR_INVALID_SIZE;
    }

    *out_buf = (unsigned char *)response.buf;
    *out_len = response.len;
    return ESP_OK;
}

static bool cap_im_wechat_is_hex_string(const unsigned char *buf, size_t len)
{
    size_t i;

    if (!buf || len == 0) {
        return false;
    }

    for (i = 0; i < len; i++) {
        if (!isxdigit(buf[i])) {
            return false;
        }
    }

    return true;
}

static esp_err_t cap_im_wechat_hex_decode(const char *hex,
                                          unsigned char *out,
                                          size_t out_size,
                                          size_t *out_len)
{
    size_t hex_len;
    size_t i;

    if (!hex || !out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    hex_len = strlen(hex);
    if ((hex_len % 2) != 0 || out_size < (hex_len / 2)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (i = 0; i < hex_len / 2; i++) {
        unsigned int value = 0;

        if (sscanf(hex + (i * 2), "%2x", &value) != 1) {
            return ESP_FAIL;
        }
        out[i] = (unsigned char)value;
    }

    *out_len = hex_len / 2;
    return ESP_OK;
}

static esp_err_t cap_im_wechat_parse_aes_key(const char *aes_key_base64,
                                             unsigned char *key_buf,
                                             size_t key_buf_size,
                                             size_t *key_len)
{
    unsigned char decoded[64];
    size_t decoded_len = 0;
    int ret;

    if (!aes_key_base64 || !aes_key_base64[0] || !key_buf || !key_len) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = mbedtls_base64_decode(decoded,
                                sizeof(decoded),
                                &decoded_len,
                                (const unsigned char *)aes_key_base64,
                                strlen(aes_key_base64));
    if (ret != 0) {
        return ESP_FAIL;
    }

    if (decoded_len == 16) {
        if (key_buf_size < 16) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(key_buf, decoded, 16);
        *key_len = 16;
        return ESP_OK;
    }

    if (decoded_len == 32 && cap_im_wechat_is_hex_string(decoded, decoded_len)) {
        return cap_im_wechat_hex_decode((const char *)decoded, key_buf, key_buf_size, key_len);
    }

    return ESP_FAIL;
}

static esp_err_t cap_im_wechat_aes_ecb_crypt(const unsigned char *input,
                                             size_t input_len,
                                             const unsigned char *key,
                                             size_t key_len,
                                             bool encrypt,
                                             unsigned char **out_buf,
                                             size_t *out_len)
{
    esp_aes_context aes;
    unsigned char *buffer = NULL;
    size_t padded_len;
    size_t i;
    int ret;

    if (!input || !key || key_len != 16 || !out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    padded_len = encrypt ? ((input_len / 16) + 1) * 16 : input_len;
    if ((padded_len % 16) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    buffer = calloc(1, padded_len);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(buffer, input, input_len);
    if (encrypt) {
        unsigned char pad = (unsigned char)(padded_len - input_len);
        memset(buffer + input_len, pad, pad);
    }

    esp_aes_init(&aes);
    ret = esp_aes_setkey(&aes, key, 128);
    if (ret != 0) {
        esp_aes_free(&aes);
        free(buffer);
        return ESP_FAIL;
    }

    for (i = 0; i < padded_len; i += 16) {
        ret = esp_aes_crypt_ecb(&aes,
                                encrypt ? ESP_AES_ENCRYPT : ESP_AES_DECRYPT,
                                buffer + i,
                                buffer + i);
        if (ret != 0) {
            esp_aes_free(&aes);
            free(buffer);
            return ESP_FAIL;
        }
    }
    esp_aes_free(&aes);

    if (!encrypt) {
        unsigned char pad = buffer[padded_len - 1];

        if (pad == 0 || pad > 16 || pad > padded_len) {
            free(buffer);
            return ESP_FAIL;
        }
        for (i = 0; i < pad; i++) {
            if (buffer[padded_len - 1 - i] != pad) {
                free(buffer);
                return ESP_FAIL;
            }
        }
        *out_len = padded_len - pad;
    } else {
        *out_len = padded_len;
    }

    *out_buf = buffer;
    return ESP_OK;
}

static esp_err_t cap_im_wechat_base64_encode(const unsigned char *input,
                                             size_t input_len,
                                             char *out,
                                             size_t out_size)
{
    size_t out_len = 0;
    int ret;

    if (!input || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = mbedtls_base64_encode((unsigned char *)out,
                                out_size,
                                &out_len,
                                input,
                                input_len);
    if (ret != 0 || out_len + 1 > out_size) {
        return ESP_FAIL;
    }

    out[out_len] = '\0';
    return ESP_OK;
}

static bool cap_im_wechat_url_char_is_unreserved(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

static esp_err_t cap_im_wechat_url_encode_dup(const char *input, char **out)
{
    static const char hex_digits[] = "0123456789ABCDEF";
    char *buf = NULL;
    char *cursor = NULL;
    size_t input_len;
    size_t i;

    if (!input || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    input_len = strlen(input);
    buf = calloc(1, input_len * 3 + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    cursor = buf;
    for (i = 0; i < input_len; i++) {
        unsigned char ch = (unsigned char)input[i];

        if (cap_im_wechat_url_char_is_unreserved(ch)) {
            *cursor++ = (char)ch;
            continue;
        }

        *cursor++ = '%';
        *cursor++ = hex_digits[(ch >> 4) & 0x0F];
        *cursor++ = hex_digits[ch & 0x0F];
    }

    *cursor = '\0';
    *out = buf;
    return ESP_OK;
}

static esp_err_t cap_im_wechat_download_media_plaintext(const char *full_url,
                                                        const char *encrypted_query_param,
                                                        const char *aes_key_base64,
                                                        size_t max_bytes,
                                                        unsigned char **out_buf,
                                                        size_t *out_len)
{
    char *url = NULL;
    char *encoded_param = NULL;
    char *fallback_url = NULL;
    unsigned char *encrypted_buf = NULL;
    size_t encrypted_len = 0;
    unsigned char key_buf[32];
    size_t key_len = 0;
    esp_err_t err;

    if (!out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    if (encrypted_query_param && encrypted_query_param[0]) {
        err = cap_im_wechat_url_encode_dup(encrypted_query_param, &encoded_param);
        if (err != ESP_OK) {
            return err;
        }
        if (asprintf(&fallback_url,
                     "%s/download?encrypted_query_param=%s",
                     s_wechat.cdn_base_url,
                     encoded_param) < 0) {
            free(encoded_param);
            return ESP_ERR_NO_MEM;
        }
        free(encoded_param);
        encoded_param = NULL;
    }

    if (full_url && full_url[0]) {
        url = strdup(full_url);
        if (!url) {
            free(fallback_url);
            return ESP_ERR_NO_MEM;
        }
    } else if (fallback_url) {
        url = fallback_url;
        fallback_url = NULL;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_wechat_download_buffer(url,
                                        max_bytes + 32,
                                        &encrypted_buf,
                                        &encrypted_len);
    free(url);
    if (err != ESP_OK) {
        if (full_url && full_url[0] && fallback_url) {
            ESP_LOGW(TAG, "download via full_url failed, retrying encrypted_query_param URL");
            err = cap_im_wechat_download_buffer(fallback_url,
                                                max_bytes + 32,
                                                &encrypted_buf,
                                                &encrypted_len);
        }
        free(fallback_url);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "download media failed");
            return err;
        }
    }
    free(fallback_url);

    if (!aes_key_base64 || !aes_key_base64[0]) {
        if (encrypted_len > max_bytes) {
            free(encrypted_buf);
            return ESP_ERR_INVALID_SIZE;
        }
        *out_buf = encrypted_buf;
        *out_len = encrypted_len;
        return ESP_OK;
    }

    err = cap_im_wechat_parse_aes_key(aes_key_base64, key_buf, sizeof(key_buf), &key_len);
    if (err != ESP_OK) {
        free(encrypted_buf);
        return err;
    }

    err = cap_im_wechat_aes_ecb_crypt(encrypted_buf,
                                      encrypted_len,
                                      key_buf,
                                      key_len,
                                      false,
                                      out_buf,
                                      out_len);
    free(encrypted_buf);
    if (err != ESP_OK) {
        return err;
    }

    if (*out_len > max_bytes) {
        free(*out_buf);
        *out_buf = NULL;
        *out_len = 0;
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cap_im_wechat_save_attachment_buffer(const char *chat_id,
                                                      const char *sender_id,
                                                      const char *message_id,
                                                      const char *attachment_kind,
                                                      const char *content_type,
                                                      const char *original_filename,
                                                      const unsigned char *buf,
                                                      size_t buf_len)
{
    char saved_dir[CAP_IM_WECHAT_PATH_BUF_SIZE];
    char saved_name[CAP_IM_WECHAT_NAME_BUF_SIZE];
    char saved_path[CAP_IM_WECHAT_PATH_BUF_SIZE];
    const char *extension = NULL;
    char *payload_json = NULL;
    esp_err_t err;

    extension = cap_im_attachment_guess_extension(original_filename,
                                                  original_filename,
                                                  content_type);
    err = cap_im_attachment_build_saved_paths(s_wechat.attachment_root_dir,
                                              "wechat",
                                              chat_id,
                                              message_id,
                                              attachment_kind,
                                              extension ? extension : ".bin",
                                              saved_dir,
                                              sizeof(saved_dir),
                                              saved_name,
                                              sizeof(saved_name),
                                              saved_path,
                                              sizeof(saved_path));
    if (err != ESP_OK) {
        return err;
    }

    ESP_RETURN_ON_ERROR(cap_im_attachment_save_buffer_to_file(TAG, saved_path, buf, buf_len),
                        TAG,
                        "save attachment buffer failed");

    payload_json = cap_im_attachment_build_payload_json(
    &(cap_im_attachment_payload_config_t) {
        .platform = "wechat",
        .attachment_kind = attachment_kind,
        .saved_path = saved_path,
        .saved_dir = saved_dir,
        .saved_name = saved_name,
        .original_filename = original_filename,
        .mime = content_type,
        .caption = NULL,
        .source_key = "message_id",
        .source_value = message_id,
        .size_bytes = buf_len,
        .saved_at_ms = cap_im_wechat_now_ms(),
    });
    if (!payload_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_wechat_publish_attachment_event(chat_id,
                                                 sender_id,
                                                 message_id,
                                                 content_type,
                                                 payload_json);
    free(payload_json);
    return err;
}

static const char *cap_im_wechat_item_aes_key(cJSON *item)
{
    cJSON *image_item = NULL;
    cJSON *file_item = NULL;
    cJSON *video_item = NULL;
    cJSON *voice_item = NULL;
    cJSON *media = NULL;
    const char *value = NULL;

    image_item = cap_im_wechat_require_object(item, "image_item");
    if (image_item) {
        value = cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(image_item, "aeskey"));
        if (value && value[0]) {
            static char encoded[96];
            unsigned char decoded[16];
            size_t decoded_len = 0;

            if (cap_im_wechat_hex_decode(value, decoded, sizeof(decoded), &decoded_len) == ESP_OK &&
                    cap_im_wechat_base64_encode(decoded, decoded_len, encoded, sizeof(encoded)) == ESP_OK) {
                return encoded;
            }
        }
        media = cap_im_wechat_require_object(image_item, "media");
        if (media) {
            return cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(media, "aes_key"));
        }
    }

    file_item = cap_im_wechat_require_object(item, "file_item");
    if (file_item) {
        media = cap_im_wechat_require_object(file_item, "media");
        if (media) {
            return cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(media, "aes_key"));
        }
    }

    video_item = cap_im_wechat_require_object(item, "video_item");
    if (video_item) {
        media = cap_im_wechat_require_object(video_item, "media");
        if (media) {
            return cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(media, "aes_key"));
        }
    }

    voice_item = cap_im_wechat_require_object(item, "voice_item");
    if (voice_item) {
        media = cap_im_wechat_require_object(voice_item, "media");
        if (media) {
            return cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(media, "aes_key"));
        }
    }

    return NULL;
}

static const char *cap_im_wechat_item_full_url(cJSON *item)
{
    cJSON *media = NULL;
    cJSON *holder = NULL;

    holder = cap_im_wechat_require_object(item, "image_item");
    if (!holder) {
        holder = cap_im_wechat_require_object(item, "file_item");
    }
    if (!holder) {
        holder = cap_im_wechat_require_object(item, "video_item");
    }
    if (!holder) {
        holder = cap_im_wechat_require_object(item, "voice_item");
    }
    if (!holder) {
        return NULL;
    }

    media = cap_im_wechat_require_object(holder, "media");
    if (!media) {
        return NULL;
    }

    return cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(media, "full_url"));
}

static const char *cap_im_wechat_item_download_param(cJSON *item)
{
    cJSON *media = NULL;
    cJSON *holder = NULL;

    holder = cap_im_wechat_require_object(item, "image_item");
    if (!holder) {
        holder = cap_im_wechat_require_object(item, "file_item");
    }
    if (!holder) {
        holder = cap_im_wechat_require_object(item, "video_item");
    }
    if (!holder) {
        holder = cap_im_wechat_require_object(item, "voice_item");
    }
    if (!holder) {
        return NULL;
    }

    media = cap_im_wechat_require_object(holder, "media");
    if (!media) {
        return NULL;
    }

    return cap_im_wechat_string_value(
               cJSON_GetObjectItemCaseSensitive(media, "encrypt_query_param"));
}

static const char *cap_im_wechat_item_file_name(cJSON *item)
{
    cJSON *file_item = cap_im_wechat_require_object(item, "file_item");

    if (!file_item) {
        return NULL;
    }

    return cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(file_item, "file_name"));
}

static esp_err_t cap_im_wechat_process_media_item(cJSON *item,
                                                  const char *chat_id,
                                                  const char *sender_id,
                                                  const char *message_id)
{
    const char *full_url = NULL;
    const char *download_param = NULL;
    const char *aes_key = NULL;
    const char *file_name = NULL;
    const char *kind = NULL;
    const char *content_type = NULL;
    unsigned char *buf = NULL;
    size_t buf_len = 0;
    esp_err_t err;
    int type;

    if (!s_wechat.enable_inbound_attachments) {
        return ESP_OK;
    }

    type = cap_im_wechat_int_value(cJSON_GetObjectItemCaseSensitive(item, "type"), 0);
    switch (type) {
    case 2:
        kind = "image";
        content_type = "image/jpeg";
        file_name = "image.jpg";
        break;
    case 3:
        kind = "voice";
        content_type = "audio/silk";
        file_name = "voice.silk";
        break;
    case 4:
        kind = "file";
        content_type = "application/octet-stream";
        file_name = cap_im_wechat_item_file_name(item);
        break;
    case 5:
        kind = "video";
        content_type = "video/mp4";
        file_name = "video.mp4";
        break;
    default:
        return ESP_OK;
    }

    full_url = cap_im_wechat_item_full_url(item);
    download_param = cap_im_wechat_item_download_param(item);
    aes_key = cap_im_wechat_item_aes_key(item);

    if ((!full_url || !full_url[0]) && (!download_param || !download_param[0])) {
        ESP_LOGW(TAG, "wechat item missing media url");
        return ESP_FAIL;
    }

    err = cap_im_wechat_download_media_plaintext(full_url,
                                                 download_param,
                                                 aes_key,
                                                 s_wechat.max_inbound_file_bytes,
                                                 &buf,
                                                 &buf_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wechat media download failed: %s", esp_err_to_name(err));
        return err;
    }

    err = cap_im_wechat_save_attachment_buffer(chat_id,
                                               sender_id,
                                               message_id,
                                               kind,
                                               content_type,
                                               file_name,
                                               buf,
                                               buf_len);
    free(buf);
    return err;
}

static void cap_im_wechat_append_text(char *buf, size_t buf_size, const char *text)
{
    if (!buf || !buf_size || !text || !text[0]) {
        return;
    }

    if (buf[0]) {
        strlcat(buf, "\n", buf_size);
    }
    strlcat(buf, text, buf_size);
}

static esp_err_t cap_im_wechat_process_message(cJSON *msg)
{
    cJSON *item_list = NULL;
    cJSON *item = NULL;
    char message_id_buf[32];
    char *text_buf = NULL;
    const char *from_user_id = NULL;
    const char *group_id = NULL;
    const char *context_token = NULL;
    const char *chat_id = NULL;
    int item_count;
    int i;

    if (!cJSON_IsObject(msg)) {
        return ESP_ERR_INVALID_ARG;
    }

    text_buf = calloc(1, CAP_IM_WECHAT_MAX_MSG_LEN + 1);
    if (!text_buf) {
        return ESP_ERR_NO_MEM;
    }

    from_user_id =
        cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(msg, "from_user_id"));
    group_id =
        cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(msg, "group_id"));
    context_token =
        cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(msg, "context_token"));
    chat_id = (group_id && group_id[0]) ? group_id : from_user_id;
    if (!chat_id || !chat_id[0]) {
        free(text_buf);
        return ESP_ERR_INVALID_RESPONSE;
    }

    snprintf(message_id_buf,
             sizeof(message_id_buf),
             "%" PRId64,
             cap_im_wechat_int64_value(cJSON_GetObjectItemCaseSensitive(msg, "message_id"),
                                       cap_im_wechat_now_ms()));
    if (cap_im_wechat_dedup_check_and_record(message_id_buf)) {
        free(text_buf);
        return ESP_OK;
    }

    cap_im_wechat_context_remember(chat_id, context_token);

    item_list = cap_im_wechat_require_array(msg, "item_list");
    if (!item_list) {
        free(text_buf);
        return ESP_OK;
    }

    item_count = cJSON_GetArraySize(item_list);
    for (i = 0; i < item_count; i++) {
        int type;
        const char *text = NULL;

        item = cJSON_GetArrayItem(item_list, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }

        type = cap_im_wechat_int_value(cJSON_GetObjectItemCaseSensitive(item, "type"), 0);
        if (type == 1) {
            cJSON *text_item = cap_im_wechat_require_object(item, "text_item");
            if (text_item) {
                text = cap_im_wechat_string_value(
                           cJSON_GetObjectItemCaseSensitive(text_item, "text"));
            }
            cap_im_wechat_append_text(text_buf, CAP_IM_WECHAT_MAX_MSG_LEN + 1, text);
        } else if (type == 2 || type == 3 || type == 4 || type == 5) {
            cap_im_wechat_process_media_item(item, chat_id, from_user_id, message_id_buf);
        }
    }

    {
        esp_err_t err = cap_im_wechat_publish_inbound_text(chat_id,
                                                           from_user_id ? from_user_id : chat_id,
                                                           message_id_buf,
                                                           text_buf);
        free(text_buf);
        return err;
    }
}

static esp_err_t cap_im_wechat_poll_once(void)
{
    cap_im_wechat_http_resp_t response = {0};
    cJSON *root = NULL;
    cJSON *msgs = NULL;
    cJSON *msg = NULL;
    esp_err_t err;
    int i;
    int msg_count;

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    if (!cJSON_AddStringToObject(root, "get_updates_buf", s_wechat.sync_buf ? s_wechat.sync_buf : "")) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    err = cap_im_wechat_add_base_info(root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    err = cap_im_wechat_api_post("ilink/bot/getupdates",
                                 root,
                                 s_wechat.poll_timeout_ms + 5000,
                                 &response);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        cap_im_wechat_resp_cleanup(&response);
        return err;
    }

    root = cJSON_Parse(response.buf);
    cap_im_wechat_resp_cleanup(&response);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (cap_im_wechat_int_value(cJSON_GetObjectItemCaseSensitive(root, "ret"), 0) != 0 ||
            cap_im_wechat_int_value(cJSON_GetObjectItemCaseSensitive(root, "errcode"), 0) != 0) {
        /* cJSON_PrintUnformatted returns a heap string; free it after logging
         * (it was previously leaked on every error poll cycle). */
        char *err_body = cJSON_PrintUnformatted(root);
        ESP_LOGW(TAG, "wechat getupdates error: %s", err_body ? err_body : "(null)");
        cJSON_free(err_body);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    s_wechat.poll_timeout_ms =
        cap_im_wechat_int_value(cJSON_GetObjectItemCaseSensitive(root,
                                                                 "longpolling_timeout_ms"),
                                CAP_IM_WECHAT_POLL_TIMEOUT_MS);
    if (s_wechat.poll_timeout_ms <= 0) {
        s_wechat.poll_timeout_ms = CAP_IM_WECHAT_POLL_TIMEOUT_MS;
    }

    {
        const char *next_sync =
            cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root,
                                                                        "get_updates_buf"));
        if (next_sync) {
            char *dup = strdup(next_sync);

            if (!dup) {
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }
            free(s_wechat.sync_buf);
            s_wechat.sync_buf = dup;
        }
    }

    msgs = cap_im_wechat_require_array(root, "msgs");
    if (!msgs) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    msg_count = cJSON_GetArraySize(msgs);
    for (i = 0; i < msg_count; i++) {
        msg = cJSON_GetArrayItem(msgs, i);
        cap_im_wechat_process_message(msg);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static void cap_im_wechat_poll_task(void *arg)
{
    (void)arg;

    while (!s_wechat.stop_requested) {
        if (!s_wechat.configured) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (cap_im_wechat_poll_once() != ESP_OK) {
            ESP_LOGW(TAG, "WeChat polling failed, retrying");
            vTaskDelay(pdMS_TO_TICKS(CAP_IM_WECHAT_RETRY_DELAY_MS));
        }
    }

    s_wechat.poll_task = NULL;
    claw_task_delete(NULL);
}

static esp_err_t cap_im_wechat_send_message_json(cJSON *msg_root)
{
    cJSON *root = NULL;
    cap_im_wechat_http_resp_t response = {0};
    esp_err_t err;

    if (!msg_root) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    if (!cJSON_AddItemToObject(root, "msg", msg_root)) {
        cJSON_Delete(msg_root);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_wechat_add_base_info(root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    err = cap_im_wechat_api_post("ilink/bot/sendmessage", root, 15000, &response);
    cJSON_Delete(root);
    cap_im_wechat_resp_cleanup(&response);
    return err;
}

static void cap_im_wechat_build_client_id(char *buf, size_t buf_size)
{
    uint32_t a = esp_random();
    uint32_t b = esp_random();

    snprintf(buf, buf_size, "espwx-%08" PRIx32 "%08" PRIx32, a, b);
}

static esp_err_t cap_im_wechat_send_text_chunk(const char *chat_id, const char *chunk)
{
    cJSON *msg = NULL;
    cJSON *item_list = NULL;
    cJSON *item = NULL;
    cJSON *text_item = NULL;
    char client_id[CAP_IM_WECHAT_CLIENT_ID_SIZE];
    const char *context_token = NULL;

    if (!chat_id || !chat_id[0] || !chunk || !chunk[0] || !s_wechat.configured) {
        return ESP_ERR_INVALID_ARG;
    }

    msg = cJSON_CreateObject();
    if (!msg) {
        return ESP_ERR_NO_MEM;
    }

    cap_im_wechat_build_client_id(client_id, sizeof(client_id));
    cJSON_AddStringToObject(msg, "from_user_id", "");
    cJSON_AddStringToObject(msg, "to_user_id", chat_id);
    cJSON_AddStringToObject(msg, "client_id", client_id);
    cJSON_AddNumberToObject(msg, "message_type", 2);
    cJSON_AddNumberToObject(msg, "message_state", 2);
    context_token = cap_im_wechat_context_lookup(chat_id);
    if (context_token) {
        cJSON_AddStringToObject(msg, "context_token", context_token);
    }

    item_list = cJSON_AddArrayToObject(msg, "item_list");
    item = cJSON_CreateObject();
    text_item = cJSON_CreateObject();
    if (!item_list || !item || !text_item) {
        cJSON_Delete(msg);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(item, "type", 1);
    cJSON_AddStringToObject(text_item, "text", chunk);
    cJSON_AddItemToObject(item, "text_item", text_item);
    cJSON_AddItemToArray(item_list, item);
    return cap_im_wechat_send_message_json(msg);
}

static esp_err_t cap_im_wechat_read_file(const char *path,
                                         unsigned char **out_buf,
                                         size_t *out_len)
{
    FILE *file = NULL;
    struct stat st = {0};
    unsigned char *buf = NULL;

    if (!path || !out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(path, &st) != 0 || st.st_size < 0) {
        return ESP_FAIL;
    }

    buf = calloc(1, (size_t)st.st_size);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    file = fopen(path, "rb");
    if (!file) {
        free(buf);
        return ESP_FAIL;
    }

    if (st.st_size > 0 &&
            fread(buf, 1, (size_t)st.st_size, file) != (size_t)st.st_size) {
        fclose(file);
        free(buf);
        return ESP_FAIL;
    }

    fclose(file);
    *out_buf = buf;
    *out_len = (size_t)st.st_size;
    return ESP_OK;
}

static esp_err_t cap_im_wechat_md5_hex(const unsigned char *buf,
                                       size_t len,
                                       char *out,
                                       size_t out_size)
{
    unsigned char digest[16];
    size_t i;

    if (!buf || !out || out_size < 33) {
        return ESP_ERR_INVALID_ARG;
    }

    md5_context_t md5_ctx;
    esp_rom_md5_init(&md5_ctx);
    esp_rom_md5_update(&md5_ctx, buf, len);
    esp_rom_md5_final(digest, &md5_ctx);
    for (i = 0; i < sizeof(digest); i++) {
        snprintf(out + (i * 2), out_size - (i * 2), "%02x", digest[i]);
    }
    return ESP_OK;
}

static esp_err_t cap_im_wechat_random_hex(unsigned char *raw,
                                          size_t raw_len,
                                          char *hex,
                                          size_t hex_size)
{
    size_t i;

    if (!raw || !hex || hex_size < (raw_len * 2 + 1)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_fill_random(raw, raw_len);
    for (i = 0; i < raw_len; i++) {
        snprintf(hex + (i * 2), hex_size - (i * 2), "%02x", raw[i]);
    }
    return ESP_OK;
}

static esp_err_t cap_im_wechat_upload_ciphertext(const char *upload_full_url,
                                                 const char *upload_param,
                                                 const char *filekey,
                                                 const unsigned char *ciphertext,
                                                 size_t ciphertext_len,
                                                 char **encrypted_param)
{
    cap_im_wechat_http_resp_t response = {0};
    char *url = NULL;
    char *encoded_upload_param = NULL;
    char *encoded_filekey = NULL;
    esp_err_t err;

    if (upload_full_url && upload_full_url[0]) {
        url = strdup(upload_full_url);
        if (!url) {
            return ESP_ERR_NO_MEM;
        }
    } else if (upload_param && upload_param[0] && filekey && filekey[0]) {
        err = cap_im_wechat_url_encode_dup(upload_param, &encoded_upload_param);
        if (err != ESP_OK) {
            return err;
        }
        err = cap_im_wechat_url_encode_dup(filekey, &encoded_filekey);
        if (err != ESP_OK) {
            free(encoded_upload_param);
            return err;
        }
        if (asprintf(&url,
                     "%s/upload?encrypted_query_param=%s&filekey=%s",
                     s_wechat.cdn_base_url,
                     encoded_upload_param,
                     encoded_filekey) < 0) {
            free(encoded_upload_param);
            free(encoded_filekey);
            return ESP_ERR_NO_MEM;
        }
        free(encoded_upload_param);
        free(encoded_filekey);
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_wechat_http_request(url,
                                     HTTP_METHOD_POST,
                                     "application/octet-stream",
                                     ciphertext,
                                     ciphertext_len,
                                     30000,
                                     false,
                                     false,
                                     &response);
    free(url);
    if (err == ESP_OK) {
        if (!response.encrypted_param || !response.encrypted_param[0]) {
            err = ESP_FAIL;
        } else {
            *encrypted_param = response.encrypted_param;
            response.encrypted_param = NULL;
        }
    }
    cap_im_wechat_resp_cleanup(&response);
    return err;
}

static esp_err_t cap_im_wechat_upload_file(const char *chat_id,
                                           const char *file_path,
                                           int media_type,
                                           char **download_param,
                                           char *aes_key_base64,
                                           size_t aes_key_base64_size,
                                           size_t *ciphertext_size,
                                           size_t *plaintext_size)
{
    unsigned char *plaintext = NULL;
    unsigned char *ciphertext = NULL;
    unsigned char aes_key_raw[16];
    char aes_key_hex[33];
    char filekey_hex[33];
    char md5_hex[33];
    char *upload_full_url = NULL;
    char *upload_param = NULL;
    char upload_aes_key_base64[96];
    size_t plain_len = 0;
    size_t cipher_len = 0;
    cap_im_wechat_http_resp_t response = {0};
    cJSON *root = NULL;
    esp_err_t err;

    if (!chat_id || !file_path || !download_param || !aes_key_base64 || !ciphertext_size ||
            !plaintext_size) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(cap_im_wechat_read_file(file_path, &plaintext, &plain_len),
                        TAG,
                        "read upload file failed");
    ESP_RETURN_ON_ERROR(cap_im_wechat_md5_hex(plaintext, plain_len, md5_hex, sizeof(md5_hex)),
                        TAG,
                        "md5 failed");
    ESP_RETURN_ON_ERROR(cap_im_wechat_random_hex(aes_key_raw,
                                                 sizeof(aes_key_raw),
                                                 aes_key_hex,
                                                 sizeof(aes_key_hex)),
                        TAG,
                        "random aes key failed");
    ESP_RETURN_ON_ERROR(cap_im_wechat_base64_encode((const unsigned char *)aes_key_hex,
                                                    strlen(aes_key_hex),
                                                    upload_aes_key_base64,
                                                    sizeof(upload_aes_key_base64)),
                        TAG,
                        "base64 aes key failed");
    {
        unsigned char filekey_raw[16];

        ESP_RETURN_ON_ERROR(cap_im_wechat_random_hex(filekey_raw,
                                                     sizeof(filekey_raw),
                                                     filekey_hex,
                                                     sizeof(filekey_hex)),
                            TAG,
                            "random filekey failed");
    }

    root = cJSON_CreateObject();
    if (!root) {
        free(plaintext);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "filekey", filekey_hex);
    cJSON_AddNumberToObject(root, "media_type", media_type);
    cJSON_AddStringToObject(root, "to_user_id", chat_id);
    cJSON_AddNumberToObject(root, "rawsize", (double)plain_len);
    cJSON_AddStringToObject(root, "rawfilemd5", md5_hex);
    cJSON_AddNumberToObject(root, "filesize", (double)(((plain_len / 16) + 1) * 16));
    cJSON_AddBoolToObject(root, "no_need_thumb", 1);
    cJSON_AddStringToObject(root, "aeskey", aes_key_hex);
    err = cap_im_wechat_add_base_info(root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        free(plaintext);
        return err;
    }

    err = cap_im_wechat_api_post("ilink/bot/getuploadurl", root, 15000, &response);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        free(plaintext);
        cap_im_wechat_resp_cleanup(&response);
        return err;
    }

    root = cJSON_Parse(response.buf);
    cap_im_wechat_resp_cleanup(&response);
    if (!root) {
        free(plaintext);
        return ESP_ERR_INVALID_RESPONSE;
    }

    upload_full_url = strdup(cap_im_wechat_string_value(
                                 cJSON_GetObjectItemCaseSensitive(root, "upload_full_url")) ?
                             cap_im_wechat_string_value(
                                 cJSON_GetObjectItemCaseSensitive(root, "upload_full_url")) :
                             "");
    upload_param = strdup(cap_im_wechat_string_value(
                              cJSON_GetObjectItemCaseSensitive(root, "upload_param")) ?
                          cap_im_wechat_string_value(
                              cJSON_GetObjectItemCaseSensitive(root, "upload_param")) :
                          "");
    cJSON_Delete(root);
    if (!upload_full_url || !upload_param) {
        free(upload_full_url);
        free(upload_param);
        free(plaintext);
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_wechat_aes_ecb_crypt(plaintext,
                                      plain_len,
                                      aes_key_raw,
                                      sizeof(aes_key_raw),
                                      true,
                                      &ciphertext,
                                      &cipher_len);
    free(plaintext);
    if (err != ESP_OK) {
        free(upload_full_url);
        free(upload_param);
        return err;
    }

    err = cap_im_wechat_upload_ciphertext(upload_full_url[0] ? upload_full_url : NULL,
                                          upload_param[0] ? upload_param : NULL,
                                          filekey_hex,
                                          ciphertext,
                                          cipher_len,
                                          download_param);
    free(upload_full_url);
    free(upload_param);
    free(ciphertext);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(aes_key_base64, upload_aes_key_base64, aes_key_base64_size);
    *ciphertext_size = cipher_len;
    *plaintext_size = plain_len;
    return ESP_OK;
}

static esp_err_t cap_im_wechat_send_image_message(const char *chat_id,
                                                  const char *download_param,
                                                  const char *aes_key_base64,
                                                  size_t ciphertext_size)
{
    cJSON *msg = NULL;
    cJSON *item_list = NULL;
    cJSON *item = NULL;
    cJSON *image_item = NULL;
    cJSON *media = NULL;
    char client_id[CAP_IM_WECHAT_CLIENT_ID_SIZE];
    const char *context_token = NULL;

    msg = cJSON_CreateObject();
    if (!msg) {
        return ESP_ERR_NO_MEM;
    }

    cap_im_wechat_build_client_id(client_id, sizeof(client_id));
    cJSON_AddStringToObject(msg, "from_user_id", "");
    cJSON_AddStringToObject(msg, "to_user_id", chat_id);
    cJSON_AddStringToObject(msg, "client_id", client_id);
    cJSON_AddNumberToObject(msg, "message_type", 2);
    cJSON_AddNumberToObject(msg, "message_state", 2);
    context_token = cap_im_wechat_context_lookup(chat_id);
    if (context_token) {
        cJSON_AddStringToObject(msg, "context_token", context_token);
    }

    item_list = cJSON_AddArrayToObject(msg, "item_list");
    item = cJSON_CreateObject();
    image_item = cJSON_CreateObject();
    media = cJSON_CreateObject();
    if (!item_list || !item || !image_item || !media) {
        cJSON_Delete(msg);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(item, "type", 2);
    cJSON_AddStringToObject(media, "encrypt_query_param", download_param);
    cJSON_AddStringToObject(media, "aes_key", aes_key_base64);
    cJSON_AddNumberToObject(media, "encrypt_type", 1);
    cJSON_AddItemToObject(image_item, "media", media);
    cJSON_AddNumberToObject(image_item, "mid_size", (double)ciphertext_size);
    cJSON_AddItemToObject(item, "image_item", image_item);
    cJSON_AddItemToArray(item_list, item);
    return cap_im_wechat_send_message_json(msg);
}

static esp_err_t cap_im_wechat_send_message_execute(const char *input_json,
                                                    const claw_cap_call_context_t *ctx,
                                                    char *output,
                                                    size_t output_size)
{
    cJSON *root = NULL;
    const char *chat_id = NULL;
    const char *message = NULL;
    const char *event_type = NULL;
    cap_im_wechat_stage_send_mode_t stage_mode;
    esp_err_t err;

    (void)ctx;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(input_json);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    chat_id = cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "chat_id"));
    message = cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "message"));
    event_type = cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "event_type"));
    if (!chat_id || !message) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    stage_mode = cap_im_wechat_stage_send_mode_for_event(chat_id, event_type);
    if (stage_mode == CAP_IM_WECHAT_STAGE_SKIP) {
        ESP_LOGI(TAG,
                 "skip agent_stage text for chat=%s after %d consecutive stage messages",
                 chat_id,
                 CAP_IM_WECHAT_STAGE_LIMIT);
        cJSON_Delete(root);
        strlcpy(output, "{\"ok\":true,\"skipped\":true}", output_size);
        return ESP_OK;
    }

    if (stage_mode == CAP_IM_WECHAT_STAGE_SEND_LIMIT_NOTICE) {
        message =
            "Due to Weixin's limitation of supporting only up to 10 output messages, any subsequent step messages will be ignored.";
    }

    err = cap_im_wechat_send_text(chat_id, message);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(output, "{\"ok\":true}", output_size);
    return ESP_OK;
}

static esp_err_t cap_im_wechat_send_image_execute(const char *input_json,
                                                  const claw_cap_call_context_t *ctx,
                                                  char *output,
                                                  size_t output_size)
{
    cJSON *root = NULL;
    const char *chat_id = NULL;
    const char *path = NULL;
    const char *caption = NULL;
    esp_err_t err;

    (void)ctx;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(input_json);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    chat_id = cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "chat_id"));
    path = cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "path"));
    caption = cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "caption"));
    if (!chat_id || !path) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_wechat_send_image(chat_id, path, caption);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(output, "{\"ok\":true}", output_size);
    return ESP_OK;
}

static esp_err_t cap_im_wechat_gateway_init(void)
{
    ESP_RETURN_ON_ERROR(cap_im_wechat_ensure_state(), TAG, "alloc state failed");

    if (cap_im_wechat_lock() == ESP_OK) {
        if (!s_wechat.qr.status[0]) {
            cap_im_wechat_qr_reset_locked();
        }
        cap_im_wechat_unlock();
    }
    return ESP_OK;
}

static esp_err_t cap_im_wechat_gateway_start(void)
{
    BaseType_t ok;

    ESP_RETURN_ON_ERROR(cap_im_wechat_ensure_state(), TAG, "alloc state failed");

    if (s_wechat.poll_task) {
        return ESP_OK;
    }

    s_wechat.stop_requested = false;
    ok = claw_task_create(&(claw_task_config_t){
                              .name = "wechat_poll",
                              .stack_size = 6144,
                              .priority = 5,
                              .core_id = tskNO_AFFINITY,
                              .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                          },
                          cap_im_wechat_poll_task,
                          NULL,
                          &s_wechat.poll_task);
    if (ok != pdPASS) {
        s_wechat.poll_task = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_im_wechat_gateway_stop(void)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    if (!s_wechat_state) {
        return ESP_OK;
    }

    s_wechat.stop_requested = true;
    s_wechat.qr.stop_requested = true;
    s_wechat.qr.active = false;
    while (s_wechat.poll_task && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    while (s_wechat.qr_task && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (s_wechat.poll_task || s_wechat.qr_task) {
        return ESP_ERR_TIMEOUT;
    }

    cap_im_wechat_free_state();
    return ESP_OK;
}

static const claw_cap_descriptor_t s_wechat_descriptors[] = {
    {
        .id = "wechat_gateway",
        .name = "wechat_gateway",
        .family = "im",
        .description = "WeChat long-poll gateway event source.",
        .kind = CLAW_CAP_KIND_EVENT_SOURCE,
        .cap_flags = CLAW_CAP_FLAG_EMITS_EVENTS |
        CLAW_CAP_FLAG_SUPPORTS_LIFECYCLE,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .init = cap_im_wechat_gateway_init,
        .start = cap_im_wechat_gateway_start,
        .stop = cap_im_wechat_gateway_stop,
    },
    {
        .id = "wechat_send_message",
        .name = "wechat_send_message",
        .family = "im",
        .description = "Send a WeChat text message.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"},\"event_type\":{\"type\":\"string\"}},\"required\":[\"chat_id\",\"message\"]}",
        .execute = cap_im_wechat_send_message_execute,
    },
    {
        .id = "wechat_send_image",
        .name = "wechat_send_image",
        .family = "im",
        .description = "Send a WeChat image from a local path.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"caption\":{\"type\":\"string\"}},\"required\":[\"chat_id\",\"path\"]}",
        .execute = cap_im_wechat_send_image_execute,
    },
};

static const claw_cap_group_t s_wechat_group = {
    .group_id = "cap_im_wechat",
    .descriptors = s_wechat_descriptors,
    .descriptor_count = sizeof(s_wechat_descriptors) / sizeof(s_wechat_descriptors[0]),
};

esp_err_t cap_im_wechat_register_group(void)
{
    if (claw_cap_group_exists(s_wechat_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_wechat_group);
}

esp_err_t cap_im_wechat_set_client_config(const cap_im_wechat_client_config_t *config)
{
    if (!config || !config->token || !config->base_url) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(cap_im_wechat_ensure_state(), TAG, "alloc state failed");

    strlcpy(s_wechat.token, config->token, sizeof(s_wechat.token));
    strlcpy(s_wechat.base_url,
            config->base_url[0] ? config->base_url : CAP_IM_WECHAT_DEFAULT_BASE_URL,
            sizeof(s_wechat.base_url));
    strlcpy(s_wechat.cdn_base_url,
            (config->cdn_base_url && config->cdn_base_url[0]) ? config->cdn_base_url :
            CAP_IM_WECHAT_DEFAULT_CDN_BASE_URL,
            sizeof(s_wechat.cdn_base_url));
    strlcpy(s_wechat.account_id,
            (config->account_id && config->account_id[0]) ? config->account_id : "default",
            sizeof(s_wechat.account_id));
    strlcpy(s_wechat.app_id,
            (config->app_id && config->app_id[0]) ? config->app_id :
            CAP_IM_WECHAT_DEFAULT_APP_ID,
            sizeof(s_wechat.app_id));
    strlcpy(s_wechat.client_version,
            (config->client_version && config->client_version[0]) ? config->client_version :
            CAP_IM_WECHAT_DEFAULT_CLIENT_VERSION,
            sizeof(s_wechat.client_version));
    strlcpy(s_wechat.route_tag,
            (config->route_tag && config->route_tag[0]) ? config->route_tag : "",
            sizeof(s_wechat.route_tag));
    s_wechat.configured = s_wechat.token[0] && s_wechat.base_url[0];
    return ESP_OK;
}

esp_err_t cap_im_wechat_set_attachment_config(
    const cap_im_wechat_attachment_config_t *config)
{
    if (!config || !config->storage_root_dir) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(cap_im_wechat_ensure_state(), TAG, "alloc state failed");

    strlcpy(s_wechat.attachment_root_dir,
            config->storage_root_dir,
            sizeof(s_wechat.attachment_root_dir));
    s_wechat.max_inbound_file_bytes = config->max_inbound_file_bytes;
    s_wechat.enable_inbound_attachments = config->enable_inbound_attachments;
    return ESP_OK;
}

esp_err_t cap_im_wechat_start(void)
{
    return cap_im_wechat_gateway_start();
}

esp_err_t cap_im_wechat_stop(void)
{
    return cap_im_wechat_gateway_stop();
}

esp_err_t cap_im_wechat_qr_login_start(const char *account_id, bool force)
{
    BaseType_t ok;
    esp_err_t err;

    err = cap_im_wechat_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (s_wechat.qr.active && !force) {
        cap_im_wechat_unlock();
        return ESP_OK;
    }

    cap_im_wechat_qr_reset_locked();
    s_wechat.qr.active = true;
    s_wechat.qr.persisted = false;
    if (account_id && account_id[0]) {
        strlcpy(s_wechat.qr.account_id, account_id, sizeof(s_wechat.qr.account_id));
    }
    cap_im_wechat_random_session_key(s_wechat.qr.session_key, sizeof(s_wechat.qr.session_key));
    err = cap_im_wechat_qr_fetch_code_locked();
    if (err != ESP_OK) {
        s_wechat.qr.active = false;
        strlcpy(s_wechat.qr.status, "error", sizeof(s_wechat.qr.status));
        snprintf(s_wechat.qr.message,
                 sizeof(s_wechat.qr.message),
                 "拉取二维码失败: %s",
                 esp_err_to_name(err));
        cap_im_wechat_unlock();
        return err;
    }

    if (!s_wechat.qr_task) {
        ok = claw_task_create(&(claw_task_config_t){
                                  .name = "wechat_qr",
                                  .stack_size = 6144,
                                  .priority = 5,
                                  .core_id = tskNO_AFFINITY,
                                  .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                              },
                              cap_im_wechat_qr_task,
                              NULL,
                              &s_wechat.qr_task);
        if (ok != pdPASS) {
            s_wechat.qr.active = false;
            s_wechat.qr_task = NULL;
            cap_im_wechat_unlock();
            return ESP_FAIL;
        }
    }

    cap_im_wechat_unlock();
    return ESP_OK;
}

esp_err_t cap_im_wechat_qr_login_get_status(cap_im_wechat_qr_login_status_t *out_status)
{
    esp_err_t err;

    if (!out_status) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(cap_im_wechat_ensure_state(), TAG, "alloc state failed");

    err = cap_im_wechat_lock();
    if (err != ESP_OK) {
        return err;
    }

    memset(out_status, 0, sizeof(*out_status));
    out_status->active = s_wechat.qr.active;
    out_status->configured = s_wechat.configured;
    out_status->completed = s_wechat.qr.completed;
    out_status->persisted = s_wechat.qr.persisted;
    strlcpy(out_status->session_key, s_wechat.qr.session_key, sizeof(out_status->session_key));
    strlcpy(out_status->status,
            s_wechat.qr.status[0] ? s_wechat.qr.status : "idle",
            sizeof(out_status->status));
    strlcpy(out_status->message, s_wechat.qr.message, sizeof(out_status->message));
    strlcpy(out_status->qr_data_url, s_wechat.qr.qr_data_url, sizeof(out_status->qr_data_url));
    strlcpy(out_status->account_id,
            s_wechat.qr.ilink_bot_id[0] ? s_wechat.qr.ilink_bot_id : s_wechat.qr.account_id,
            sizeof(out_status->account_id));
    strlcpy(out_status->user_id, s_wechat.qr.ilink_user_id, sizeof(out_status->user_id));
    strlcpy(out_status->token, s_wechat.qr.bot_token, sizeof(out_status->token));
    strlcpy(out_status->base_url,
            s_wechat.qr.base_url[0] ? s_wechat.qr.base_url : CAP_IM_WECHAT_DEFAULT_BASE_URL,
            sizeof(out_status->base_url));
    cap_im_wechat_unlock();
    return ESP_OK;
}

esp_err_t cap_im_wechat_qr_login_cancel(void)
{
    esp_err_t err = cap_im_wechat_lock();

    if (err != ESP_OK) {
        return err;
    }

    s_wechat.qr.stop_requested = true;
    s_wechat.qr.active = false;
    strlcpy(s_wechat.qr.status, "cancelled", sizeof(s_wechat.qr.status));
    strlcpy(s_wechat.qr.message, "已取消微信登录。", sizeof(s_wechat.qr.message));
    cap_im_wechat_unlock();
    return ESP_OK;
}

esp_err_t cap_im_wechat_qr_login_mark_persisted(void)
{
    esp_err_t err = cap_im_wechat_lock();

    if (err != ESP_OK) {
        return err;
    }

    s_wechat.qr.persisted = true;
    cap_im_wechat_unlock();
    return ESP_OK;
}

esp_err_t cap_im_wechat_send_text(const char *chat_id, const char *text)
{
    size_t len;
    size_t offset = 0;
    esp_err_t last_err = ESP_OK;

    ESP_RETURN_ON_ERROR(cap_im_wechat_ensure_state(), TAG, "alloc state failed");

    if (!chat_id || !chat_id[0] || !text || !text[0] || !s_wechat.configured) {
        return ESP_ERR_INVALID_ARG;
    }

    len = strlen(text);
    while (offset < len) {
        size_t chunk_len = len - offset;
        char *chunk = NULL;
        esp_err_t err;

        if (chunk_len > CAP_IM_WECHAT_MAX_MSG_LEN) {
            chunk_len = claw_utils_utf8_prefix_len(text + offset, CAP_IM_WECHAT_MAX_MSG_LEN);
            if (chunk_len == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }

        chunk = calloc(1, chunk_len + 1);
        if (!chunk) {
            return ESP_ERR_NO_MEM;
        }

        memcpy(chunk, text + offset, chunk_len);
        err = cap_im_wechat_send_text_chunk(chat_id, chunk);
        free(chunk);
        if (err != ESP_OK) {
            last_err = err;
        }
        offset += chunk_len;
    }

    return last_err;
}

esp_err_t cap_im_wechat_send_image(const char *chat_id, const char *path, const char *caption)
{
    char *download_param = NULL;
    char aes_key_base64[96];
    size_t ciphertext_size = 0;
    size_t plaintext_size = 0;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(cap_im_wechat_ensure_state(), TAG, "alloc state failed");

    if (!chat_id || !chat_id[0] || !path || !path[0] || !s_wechat.configured) {
        return ESP_ERR_INVALID_ARG;
    }

    if (caption && caption[0]) {
        ESP_RETURN_ON_ERROR(cap_im_wechat_send_text(chat_id, caption), TAG, "send caption failed");
    }

    err = cap_im_wechat_upload_file(chat_id,
                                    path,
                                    1,
                                    &download_param,
                                    aes_key_base64,
                                    sizeof(aes_key_base64),
                                    &ciphertext_size,
                                    &plaintext_size);
    if (err != ESP_OK) {
        return err;
    }

    (void)plaintext_size;
    err = cap_im_wechat_send_image_message(chat_id,
                                           download_param,
                                           aes_key_base64,
                                           ciphertext_size);
    free(download_param);
    return err;
}

static void cap_im_wechat_qr_reset_locked(void)
{
    memset(&s_wechat.qr, 0, sizeof(s_wechat.qr));
    strlcpy(s_wechat.qr.status, "idle", sizeof(s_wechat.qr.status));
}

static esp_err_t cap_im_wechat_qr_fetch_code_locked(void)
{
    cap_im_wechat_http_resp_t response = {0};
    cJSON *root = NULL;
    char endpoint[96];
    const char *qrcode = NULL;
    const char *qrcode_img_content = NULL;
    esp_err_t err;

    snprintf(endpoint, sizeof(endpoint), "ilink/bot/get_bot_qrcode?bot_type=3");
    err = cap_im_wechat_http_get_json(CAP_IM_WECHAT_DEFAULT_BASE_URL,
                                      endpoint,
                                      CAP_IM_WECHAT_QR_START_TIMEOUT_MS,
                                      &response);
    if (err != ESP_OK) {
        cap_im_wechat_resp_cleanup(&response);
        return err;
    }

    root = cJSON_Parse(response.buf);
    cap_im_wechat_resp_cleanup(&response);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    qrcode = cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "qrcode"));
    qrcode_img_content = cap_im_wechat_string_value(
                             cJSON_GetObjectItemCaseSensitive(root, "qrcode_img_content"));
    if (!qrcode || !qrcode_img_content) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    strlcpy(s_wechat.qr.qrcode, qrcode, sizeof(s_wechat.qr.qrcode));
    strlcpy(s_wechat.qr.qr_data_url, qrcode_img_content, sizeof(s_wechat.qr.qr_data_url));
    strlcpy(s_wechat.qr.status, "waiting_scan", sizeof(s_wechat.qr.status));
    strlcpy(s_wechat.qr.message, "使用微信扫描二维码完成登录。", sizeof(s_wechat.qr.message));
    s_wechat.qr.started_at_ms = cap_im_wechat_now_ms();
    strlcpy(s_wechat.qr.current_api_base_url,
            CAP_IM_WECHAT_DEFAULT_BASE_URL,
            sizeof(s_wechat.qr.current_api_base_url));
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_im_wechat_qr_poll_once_locked(void)
{
    cap_im_wechat_http_resp_t response = {0};
    cJSON *root = NULL;
    char endpoint[CAP_IM_WECHAT_URL_BUF_SIZE];
    const char *status = NULL;
    const char *redirect_host = NULL;
    const char *bot_token = NULL;
    const char *ilink_bot_id = NULL;
    const char *ilink_user_id = NULL;
    const char *baseurl = NULL;
    esp_err_t err;

    if (!s_wechat.qr.active || !s_wechat.qr.qrcode[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(endpoint,
             sizeof(endpoint),
             "ilink/bot/get_qrcode_status?qrcode=%s",
             s_wechat.qr.qrcode);
    err = cap_im_wechat_http_get_json(s_wechat.qr.current_api_base_url[0] ?
                                      s_wechat.qr.current_api_base_url :
                                      CAP_IM_WECHAT_DEFAULT_BASE_URL,
                                      endpoint,
                                      CAP_IM_WECHAT_QR_POLL_TIMEOUT_MS,
                                      &response);
    if (err != ESP_OK) {
        cap_im_wechat_resp_cleanup(&response);
        return err;
    }

    root = cJSON_Parse(response.buf);
    cap_im_wechat_resp_cleanup(&response);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    status = cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "status"));
    redirect_host =
        cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "redirect_host"));
    bot_token = cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "bot_token"));
    ilink_bot_id =
        cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "ilink_bot_id"));
    ilink_user_id =
        cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "ilink_user_id"));
    baseurl = cap_im_wechat_string_value(cJSON_GetObjectItemCaseSensitive(root, "baseurl"));

    if (!status) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (strcmp(status, "wait") == 0) {
        strlcpy(s_wechat.qr.status, "waiting_scan", sizeof(s_wechat.qr.status));
        strlcpy(s_wechat.qr.message, "等待扫码。", sizeof(s_wechat.qr.message));
    } else if (strcmp(status, "scanned") == 0) {
        strlcpy(s_wechat.qr.status, "scanned", sizeof(s_wechat.qr.status));
        strlcpy(s_wechat.qr.message, "已扫码，请在微信中确认。", sizeof(s_wechat.qr.message));
    } else if (strcmp(status, "scaned_but_redirect") == 0) {
        if (redirect_host && redirect_host[0]) {
            snprintf(s_wechat.qr.current_api_base_url,
                     sizeof(s_wechat.qr.current_api_base_url),
                     "https://%s",
                     redirect_host);
        }
        strlcpy(s_wechat.qr.status, "redirected", sizeof(s_wechat.qr.status));
        strlcpy(s_wechat.qr.message, "登录节点已切换，继续等待确认。", sizeof(s_wechat.qr.message));
    } else if (strcmp(status, "expired") == 0) {
        strlcpy(s_wechat.qr.status, "expired", sizeof(s_wechat.qr.status));
        strlcpy(s_wechat.qr.message, "二维码已过期。", sizeof(s_wechat.qr.message));
        cJSON_Delete(root);
        return ESP_ERR_TIMEOUT;
    } else if (strcmp(status, "confirmed") == 0) {
        strlcpy(s_wechat.qr.status, "confirmed", sizeof(s_wechat.qr.status));
        strlcpy(s_wechat.qr.message, "微信登录成功。", sizeof(s_wechat.qr.message));
        if (bot_token) {
            strlcpy(s_wechat.qr.bot_token, bot_token, sizeof(s_wechat.qr.bot_token));
        }
        if (ilink_bot_id) {
            strlcpy(s_wechat.qr.ilink_bot_id, ilink_bot_id, sizeof(s_wechat.qr.ilink_bot_id));
        }
        if (ilink_user_id) {
            strlcpy(s_wechat.qr.ilink_user_id, ilink_user_id, sizeof(s_wechat.qr.ilink_user_id));
        }
        if (baseurl && baseurl[0]) {
            strlcpy(s_wechat.qr.base_url, baseurl, sizeof(s_wechat.qr.base_url));
        } else {
            strlcpy(s_wechat.qr.base_url,
                    s_wechat.qr.current_api_base_url[0] ? s_wechat.qr.current_api_base_url :
                    CAP_IM_WECHAT_DEFAULT_BASE_URL,
                    sizeof(s_wechat.qr.base_url));
        }
        s_wechat.qr.completed = true;
        s_wechat.qr.active = false;
    } else {
        strlcpy(s_wechat.qr.status, "error", sizeof(s_wechat.qr.status));
        strlcpy(s_wechat.qr.message, "二维码状态未知。", sizeof(s_wechat.qr.message));
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static void cap_im_wechat_qr_task(void *arg)
{
    (void)arg;

    while (1) {
        bool should_stop = false;
        bool should_break = false;
        bool needs_refresh = false;
        esp_err_t err;

        if (cap_im_wechat_lock() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        should_stop = s_wechat.qr.stop_requested || !s_wechat.qr.active;
        if (should_stop) {
            s_wechat.qr_task = NULL;
            cap_im_wechat_unlock();
            break;
        }

        if (cap_im_wechat_now_ms() - s_wechat.qr.started_at_ms > CAP_IM_WECHAT_QR_TTL_MS) {
            if (s_wechat.qr.refresh_count + 1 >= CAP_IM_WECHAT_QR_MAX_REFRESH) {
                strlcpy(s_wechat.qr.status, "expired", sizeof(s_wechat.qr.status));
                strlcpy(s_wechat.qr.message,
                        "二维码已多次过期，请重新生成。",
                        sizeof(s_wechat.qr.message));
                s_wechat.qr.active = false;
                s_wechat.qr_task = NULL;
                cap_im_wechat_unlock();
                break;
            }

            s_wechat.qr.refresh_count++;
            cap_im_wechat_unlock();
            err = cap_im_wechat_qr_fetch_code_locked();
            if (err != ESP_OK) {
                if (cap_im_wechat_lock() == ESP_OK) {
                    strlcpy(s_wechat.qr.status, "error", sizeof(s_wechat.qr.status));
                    strlcpy(s_wechat.qr.message, "刷新二维码失败。", sizeof(s_wechat.qr.message));
                    s_wechat.qr.active = false;
                    s_wechat.qr_task = NULL;
                    cap_im_wechat_unlock();
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        cap_im_wechat_unlock();
        err = cap_im_wechat_qr_poll_once_locked();

        if (cap_im_wechat_lock() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (err == ESP_ERR_TIMEOUT) {
            if (s_wechat.qr.refresh_count + 1 >= CAP_IM_WECHAT_QR_MAX_REFRESH) {
                s_wechat.qr.active = false;
                s_wechat.qr_task = NULL;
                cap_im_wechat_unlock();
                break;
            }
            s_wechat.qr.refresh_count++;
            needs_refresh = true;
        } else if (err != ESP_OK) {
            strlcpy(s_wechat.qr.status, "error", sizeof(s_wechat.qr.status));
            snprintf(s_wechat.qr.message,
                     sizeof(s_wechat.qr.message),
                     "轮询扫码状态失败: %s",
                     esp_err_to_name(err));
            s_wechat.qr.active = false;
            should_break = true;
        } else if (!s_wechat.qr.active) {
            should_break = true;
        }

        cap_im_wechat_unlock();

        if (needs_refresh) {
            err = cap_im_wechat_qr_fetch_code_locked();
            if (err != ESP_OK) {
                if (cap_im_wechat_lock() == ESP_OK) {
                    strlcpy(s_wechat.qr.status, "error", sizeof(s_wechat.qr.status));
                    strlcpy(s_wechat.qr.message, "刷新二维码失败。", sizeof(s_wechat.qr.message));
                    s_wechat.qr.active = false;
                    s_wechat.qr_task = NULL;
                    cap_im_wechat_unlock();
                }
                break;
            }
        }

        if (should_break) {
            if (cap_im_wechat_lock() == ESP_OK) {
                s_wechat.qr_task = NULL;
                cap_im_wechat_unlock();
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    claw_task_delete(NULL);
}
