/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core.h"
#include "claw_task.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "claw_core_llm.h"
#include "claw_event_publisher.h"
#include "llm/claw_llm_http_transport.h"
#include "sdkconfig.h"

static const char *TAG = "claw_core";

#define CLAW_CORE_DEFAULT_STACK_SIZE      (8 * 1024)
#define CLAW_CORE_DEFAULT_PRIORITY        5
#define CLAW_CORE_DEFAULT_CORE            tskNO_AFFINITY
#define CLAW_CORE_DEFAULT_REQUEST_Q       4
#define CLAW_CORE_DEFAULT_RESPONSE_Q      4
#define CLAW_CORE_DEFAULT_TOOL_ITERATIONS 10
#define CLAW_CORE_LOG_SNIPPET_LEN         96
#define CLAW_CORE_TOOL_SUMMARY_MAX_LEN    768

typedef struct {
    claw_core_request_t view;
    char *owned_session_id;
    char *owned_user_text;
    char *owned_source_channel;
    char *owned_source_chat_id;
    char *owned_source_sender_id;
    char *owned_source_message_id;
    char *owned_source_cap;
    char *owned_target_channel;
    char *owned_target_chat_id;
} claw_core_request_item_t;

typedef struct {
    claw_core_response_t view;
} claw_core_response_item_t;

typedef struct claw_core_pending_response {
    claw_core_response_item_t item;
    struct claw_core_pending_response *next;
} claw_core_pending_response_t;

typedef struct {
    bool initialized;
    bool started;
    char *system_prompt;
    claw_core_append_session_turn_fn append_session_turn;
    void *append_session_turn_user_ctx;
    claw_core_request_start_fn on_request_start;
    void *on_request_start_user_ctx;
    claw_core_stage_note_fn collect_stage_note;
    void *collect_stage_note_user_ctx;
    claw_core_call_cap_fn call_cap;
    void *cap_user_ctx;
    claw_core_context_provider_t *context_providers;
    size_t context_provider_count;
    size_t context_provider_capacity;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
    BaseType_t task_core;
    uint32_t max_tool_iterations;
    QueueHandle_t request_queue;
    QueueHandle_t response_queue;
    TaskHandle_t task_handle;
    SemaphoreHandle_t response_lock;
    claw_core_pending_response_t *pending_head;
    claw_core_pending_response_t *pending_tail;
    SemaphoreHandle_t inflight_lock;
    uint32_t inflight_request_id;
    volatile bool inflight_abort;
#define CLAW_CORE_MAX_COMPLETION_OBSERVERS 4
    struct {
        claw_core_completion_observer_fn fn;
        void *user_ctx;
    } completion_observers[CLAW_CORE_MAX_COMPLETION_OBSERVERS];
    size_t completion_observer_count;
} claw_core_state_t;

static claw_core_state_t *s_core = NULL;

static char *dup_string(const char *src)
{
    if (!src) {
        return NULL;
    }

    return strdup(src);
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

static const char *log_snippet(const char *text)
{
    return text ? text : "";
}

static const char *context_kind_to_string(claw_core_context_kind_t kind)
{
    switch (kind) {
    case CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT:
        return "system_prompt";
    case CLAW_CORE_CONTEXT_KIND_MESSAGES:
        return "messages";
    case CLAW_CORE_CONTEXT_KIND_TOOLS:
        return "tools";
    default:
        return "unknown";
    }
}

static esp_err_t append_tool_summary_line(char *summary,
                                          size_t summary_size,
                                          const char *tool_name,
                                          bool ok)
{
    size_t used;
    int written;

    if (!summary || summary_size == 0 || !tool_name || !tool_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    used = strlen(summary);
    if (used >= summary_size - 1) {
        return ESP_ERR_NO_MEM;
    }

    written = snprintf(summary + used,
                       summary_size - used,
                       "%s- %s: %s\n",
                       used == 0 ? "[tool_calls]\n" : "",
                       tool_name,
                       ok ? "ok" : "failed");
    if (written < 0 || (size_t)written >= summary_size - used) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

#define CLAW_CORE_OBS_CSV_MAX 384

static bool obs_csv_contains(const char *csv, const char *name)
{
    const char *needle = name;
    const char *p = csv;
    size_t need_len;

    if (!csv || !name) {
        return false;
    }
    need_len = strlen(needle);
    while (*p) {
        if (strncmp(p, needle, need_len) == 0 && (p[need_len] == ',' || p[need_len] == '\0')) {
            return true;
        }
        const char *next = strchr(p, ',');
        if (!next) {
            break;
        }
        p = next + 1;
    }
    return false;
}

static void obs_csv_append(char *csv, size_t csv_size, const char *name, bool dedup)
{
    size_t cur;
    int written;

    if (!csv || csv_size == 0 || !name || !name[0]) {
        return;
    }
    if (dedup && obs_csv_contains(csv, name)) {
        return;
    }
    cur = strlen(csv);
    if (cur >= csv_size - 1) {
        return;
    }
    written = snprintf(csv + cur, csv_size - cur, "%s%s", cur == 0 ? "" : ",", name);
    if (written < 0 || (size_t)written >= csv_size - cur) {
        csv[csv_size - 1] = '\0';
    }
}

static void log_tool_call_names(uint32_t request_id, const claw_core_llm_response_t *response)
{
    char buf[192] = {0};
    size_t off = 0;
    size_t i;

    if (!response || response->tool_call_count == 0) {
        return;
    }

    for (i = 0; i < response->tool_call_count; i++) {
        const char *name = response->tool_calls[i].name ? response->tool_calls[i].name : "(null)";
        int written = snprintf(buf + off,
                               sizeof(buf) - off,
                               "%s%s",
                               i == 0 ? "" : ",",
                               name);

        if (written < 0 || (size_t)written >= sizeof(buf) - off) {
            off = sizeof(buf) - 1;
            break;
        }
        off += (size_t)written;
    }

    ESP_LOGD(TAG, "llm_tool_calls request=%" PRIu32 " count=%u names=%s%s",
             request_id,
             (unsigned)response->tool_call_count,
             buf,
             off >= sizeof(buf) - 1 ? "..." : "");
}

static void free_context_provider_storage(void)
{
    size_t i;

    if (!s_core) {
        return;
    }

    for (i = 0; i < s_core->context_provider_count; i++) {
        free((char *)s_core->context_providers[i].name);
        s_core->context_providers[i].name = NULL;
    }
    free(s_core->context_providers);
    s_core->context_providers = NULL;
    s_core->context_provider_count = 0;
    s_core->context_provider_capacity = 0;
}

static void claw_core_free_state_storage(void)
{
    free(s_core);
    s_core = NULL;
}

static void claw_core_reset_runtime(void)
{
    if (!s_core) {
        return;
    }

    free_context_provider_storage();
    free(s_core->system_prompt);
    if (s_core->request_queue) {
        vQueueDelete(s_core->request_queue);
    }
    if (s_core->response_queue) {
        vQueueDelete(s_core->response_queue);
    }
    if (s_core->response_lock) {
        vSemaphoreDelete(s_core->response_lock);
    }
    if (s_core->inflight_lock) {
        vSemaphoreDelete(s_core->inflight_lock);
    }
    memset(s_core, 0, sizeof(*s_core));
    claw_core_free_state_storage();
}

static void free_request_item(claw_core_request_item_t *item)
{
    if (!item) {
        return;
    }

    free(item->owned_session_id);
    free(item->owned_user_text);
    free(item->owned_source_channel);
    free(item->owned_source_chat_id);
    free(item->owned_source_sender_id);
    free(item->owned_source_message_id);
    free(item->owned_source_cap);
    free(item->owned_target_channel);
    free(item->owned_target_chat_id);
    memset(item, 0, sizeof(*item));
}

static void free_response_item(claw_core_response_item_t *item)
{
    if (!item) {
        return;
    }

    free(item->view.target_channel);
    free(item->view.target_chat_id);
    free(item->view.text);
    free(item->view.error_message);
    memset(item, 0, sizeof(*item));
}

static esp_err_t push_response(claw_core_response_item_t *item)
{
    if (xQueueSend(s_core->response_queue, item, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    item->view.target_channel = NULL;
    item->view.target_chat_id = NULL;
    item->view.text = NULL;
    item->view.error_message = NULL;
    return ESP_OK;
}

static esp_err_t enqueue_pending_response(claw_core_response_item_t *item)
{
    claw_core_pending_response_t *node = calloc(1, sizeof(*node));

    if (!node) {
        return ESP_ERR_NO_MEM;
    }

    node->item = *item;
    if (!s_core->pending_tail) {
        s_core->pending_head = node;
    } else {
        s_core->pending_tail->next = node;
    }
    s_core->pending_tail = node;
    memset(item, 0, sizeof(*item));
    return ESP_OK;
}

static bool pop_pending_response(uint32_t request_id,
                                 bool match_any,
                                 claw_core_response_item_t *out_item)
{
    claw_core_pending_response_t *prev = NULL;
    claw_core_pending_response_t *cur = s_core->pending_head;

    while (cur) {
        if (match_any || cur->item.view.request_id == request_id) {
            if (prev) {
                prev->next = cur->next;
            } else {
                s_core->pending_head = cur->next;
            }
            if (s_core->pending_tail == cur) {
                s_core->pending_tail = prev;
            }
            *out_item = cur->item;
            free(cur);
            return true;
        }
        prev = cur;
        cur = cur->next;
    }

    return false;
}

static void move_response_item(claw_core_response_t *dst, claw_core_response_item_t *src)
{
    memset(dst, 0, sizeof(*dst));
    *dst = src->view;
    memset(src, 0, sizeof(*src));
}

static int64_t claw_core_now_ms(void)
{
    struct timeval tv = {0};

    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000LL) + (tv.tv_usec / 1000LL);
}

static void claw_core_check_timezone(void)
{
    const char *timezone = getenv("TZ");

    if (!timezone || timezone[0] == '\0') {
        ESP_LOGW(TAG, "Timezone is not configured; time-related responses may use an unexpected timezone");
    }
}

static esp_err_t build_response_payload_json(const claw_core_request_t *request,
                                             const claw_core_response_t *response,
                                             char **out_payload_json)
{
    cJSON *root = NULL;
    char *payload_json = NULL;

    if (!request || !response || !out_payload_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_payload_json = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    if (!cJSON_AddNumberToObject(root, "request_id", (double)request->request_id) ||
            !cJSON_AddStringToObject(root,
                                     "status",
                                     response->status == CLAW_CORE_RESPONSE_STATUS_OK ? "ok" : "error")) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (response->error_message && response->error_message[0] &&
            !cJSON_AddStringToObject(root, "error_message", response->error_message)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    payload_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload_json) {
        return ESP_ERR_NO_MEM;
    }

    *out_payload_json = payload_json;
    return ESP_OK;
}

static esp_err_t build_out_message_event_common(const char *event_id_prefix,
                                                const char *event_type,
                                                uint32_t request_id,
                                                int64_t now_ms,
                                                const char *channel,
                                                const char *chat_id,
                                                const char *text,
                                                claw_event_t *out_event)
{
    if (!event_id_prefix || !event_id_prefix[0] || !event_type || !event_type[0] ||
            !channel || !channel[0] || !chat_id || !chat_id[0] ||
            !text || !text[0] || !out_event) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_event, 0, sizeof(*out_event));
    snprintf(out_event->event_id, sizeof(out_event->event_id),
             "%s-%" PRIu32 "-%" PRId64, event_id_prefix, request_id, now_ms);
    strlcpy(out_event->source_cap, "claw_core", sizeof(out_event->source_cap));
    strlcpy(out_event->event_type, event_type, sizeof(out_event->event_type));
    strlcpy(out_event->source_channel, channel, sizeof(out_event->source_channel));
    strlcpy(out_event->chat_id, chat_id, sizeof(out_event->chat_id));
    strlcpy(out_event->content_type, "text", sizeof(out_event->content_type));
    out_event->text = (char *)text;
    out_event->timestamp_ms = now_ms;
    out_event->session_policy = CLAW_EVENT_SESSION_POLICY_CHAT;
    return ESP_OK;
}

static esp_err_t build_agent_out_message_event(const claw_core_request_t *request,
                                               const claw_core_response_t *response,
                                               claw_event_t *out_event,
                                               char **out_payload_json)
{
    const char *channel = NULL;
    const char *chat_id = NULL;
    const char *text = NULL;
    int64_t now_ms;
    esp_err_t err;

    if (!request || !response || !out_event || !out_payload_json) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_payload_json = NULL;

    text = (response->status == CLAW_CORE_RESPONSE_STATUS_OK) ?
           response->text : response->error_message;
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    err = build_response_payload_json(request, response, out_payload_json);
    if (err != ESP_OK) {
        return err;
    }

    now_ms = claw_core_now_ms();
    channel = (response->target_channel && response->target_channel[0]) ?
              response->target_channel : request->source_channel;
    chat_id = (response->target_chat_id && response->target_chat_id[0]) ?
              response->target_chat_id : request->source_chat_id;

    err = build_out_message_event_common("agent",
                                         "out_message",
                                         request->request_id,
                                         now_ms,
                                         channel,
                                         chat_id,
                                         text,
                                         out_event);
    if (err != ESP_OK) {
        free(*out_payload_json);
        *out_payload_json = NULL;
        return err;
    }

    snprintf(out_event->message_id, sizeof(out_event->message_id),
             "agent-%" PRIu32,
             request->request_id);
    if (request->source_message_id && request->source_message_id[0]) {
        strlcpy(out_event->correlation_id,
                request->source_message_id,
                sizeof(out_event->correlation_id));
    } else {
        snprintf(out_event->correlation_id, sizeof(out_event->correlation_id),
                 "%" PRIu32,
                 request->request_id);
    }
    out_event->payload_json = *out_payload_json;

    return ESP_OK;
}

static void publish_out_message_if_requested(const claw_core_request_item_t *request,
                                             const claw_core_response_item_t *response)
{
    claw_event_t event = {0};
    char *payload_json = NULL;
    esp_err_t err;

    if (!request || !response ||
            !(request->view.flags & CLAW_CORE_REQUEST_FLAG_PUBLISH_OUT_MESSAGE)) {
        return;
    }

    err = build_agent_out_message_event(&request->view,
                                        &response->view,
                                        &event,
                                        &payload_json);
    if (err == ESP_OK) {
        err = claw_event_router_publish(&event);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to publish out_message for request_id=%" PRIu32 ": %s",
                 request->view.request_id,
                 esp_err_to_name(err));
    }

    free(payload_json);
}
#if CONFIG_CLAW_CORE_STAGE_VERBOSITY_VERBOSE
static void publish_stage_event(const claw_core_request_t *request, const char *text)
{
    claw_event_t event = {0};
    int64_t now_ms;
    esp_err_t err;

    if (!request || !text || !text[0]) {
        return;
    }

    now_ms = claw_core_now_ms();
    err = build_out_message_event_common(
        "stage",
        "agent_stage",
        request->request_id,
        now_ms,
        (request->target_channel && request->target_channel[0]) ?
            request->target_channel : request->source_channel,
        (request->target_chat_id && request->target_chat_id[0]) ?
            request->target_chat_id : request->source_chat_id,
        text,
        &event);
    if (err != ESP_OK) {
        return;
    }

    esp_err_t pub_err = claw_event_router_publish(&event);
    if (pub_err != ESP_OK) {
        ESP_LOGW(TAG, "request=%" PRIu32 " failed to publish stage event: %s",
                 request->request_id, esp_err_to_name(pub_err));
    }
}
#endif

static void publish_stage_tool_calls(const claw_core_request_t *request,
                                     const claw_core_llm_response_t *response,
                                     uint32_t iteration)
{
#if CONFIG_CLAW_CORE_STAGE_VERBOSITY_VERBOSE
    char buf[256];
    size_t off = 0;
    size_t i;
    int written;

    if (!response || response->tool_call_count == 0) {
        return;
    }

    written = snprintf(buf, sizeof(buf), "🦞 [Round %" PRIu32 "] Snap: ", iteration + 1);

    if (written < 0 || (size_t)written >= sizeof(buf)) {
        return;
    }
    off = (size_t)written;

    for (i = 0; i < response->tool_call_count; i++) {
        const char *name = response->tool_calls[i].name ? response->tool_calls[i].name : "?";
        const char *args = response->tool_calls[i].arguments_json;
        if (args && args[0]) {
            written = snprintf(buf + off, sizeof(buf) - off, "%s%s(%.40s%s)",
                               i == 0 ? "" : ", ", name, args,
                               strlen(args) > 40 ? "..." : "");
        } else {
            written = snprintf(buf + off, sizeof(buf) - off, "%s%s",
                               i == 0 ? "" : ", ", name);
        }
        if (written < 0 || (size_t)written >= sizeof(buf) - off) {
            break;
        }
        off += (size_t)written;
    }

    publish_stage_event(request, buf);
#else
    (void)request;
    (void)response;
    (void)iteration;
#endif
}

static void publish_stage_note_for_round(const claw_core_request_t *request,
                                         uint32_t round_index)
{
    char *stage_note = NULL;
    esp_err_t err;

    if (!s_core->collect_stage_note) {
        return;
    }

    err = s_core->collect_stage_note(request, &stage_note, s_core->collect_stage_note_user_ctx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stage_note callback failed: %s", esp_err_to_name(err));
        free(stage_note);
        return;
    }

#if CONFIG_CLAW_CORE_STAGE_VERBOSITY_VERBOSE
    char buf[256];
    int written;

    if (!stage_note || !stage_note[0]) {
        free(stage_note);
        return;
    }

    written = snprintf(buf, sizeof(buf), "🦞 [Round %" PRIu32 "] %s", round_index + 1, stage_note);
    if (written < 0 || (size_t)written >= sizeof(buf)) {
        free(stage_note);
        return;
    }
    publish_stage_event(request, buf);
#else
    (void)request;
    (void)round_index;
#endif
    free(stage_note);
}

static esp_err_t append_user_message(cJSON *messages, const char *text)
{
    cJSON *user_msg = NULL;

    if (!messages || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    user_msg = cJSON_CreateObject();
    if (!user_msg) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", text);
    cJSON_AddItemToArray(messages, user_msg);
    return ESP_OK;
}

static esp_err_t append_message_array_json(cJSON *messages, const char *json_text)
{
    cJSON *parsed = NULL;
    cJSON *item = NULL;

    if (!messages || !json_text || !json_text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    parsed = cJSON_Parse(json_text);
    if (!parsed || !cJSON_IsArray(parsed)) {
        cJSON_Delete(parsed);
        return ESP_FAIL;
    }

    cJSON_ArrayForEach(item, parsed) {
        cJSON *dup = cJSON_Duplicate(item, true);

        if (!dup) {
            cJSON_Delete(parsed);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(messages, dup);
    }

    cJSON_Delete(parsed);
    return ESP_OK;
}

static esp_err_t append_message_array(cJSON *messages, const cJSON *items)
{
    const cJSON *item = NULL;

    if (!messages || !items || !cJSON_IsArray((cJSON *)items)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_ArrayForEach(item, items) {
        cJSON *dup = cJSON_Duplicate((cJSON *)item, true);

        if (!dup) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(messages, dup);
    }

    return ESP_OK;
}

static esp_err_t append_tool_array_json(cJSON *tools, const char *json_text)
{
    cJSON *parsed = NULL;
    cJSON *item = NULL;

    if (!tools || !json_text || !json_text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    parsed = cJSON_Parse(json_text);
    if (!parsed || !cJSON_IsArray(parsed)) {
        cJSON_Delete(parsed);
        return ESP_FAIL;
    }

    cJSON_ArrayForEach(item, parsed) {
        cJSON *dup = cJSON_Duplicate(item, true);

        if (!dup) {
            cJSON_Delete(parsed);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(tools, dup);
    }

    cJSON_Delete(parsed);
    return ESP_OK;
}

static char *build_current_turn_prompt(const claw_core_request_t *request)
{
    size_t total_len;
    char *text = NULL;

    if (!request) {
        return NULL;
    }

    static const char *k_behavior_note =
        "The agent result will be automatically sent to the user. "
        "so it is generally not need to activate cap_im_xx to return messages.\n";

    total_len = 256 + strlen(k_behavior_note);
    total_len += request->source_cap ? strlen(request->source_cap) : 0;
    total_len += request->source_channel ? strlen(request->source_channel) : 0;
    total_len += request->source_chat_id ? strlen(request->source_chat_id) : 0;
    total_len += request->source_sender_id ? strlen(request->source_sender_id) : 0;
    total_len += request->source_message_id ? strlen(request->source_message_id) : 0;

    text = calloc(1, total_len);
    if (!text) {
        return NULL;
    }

    snprintf(text,
             total_len,
             "## Current Turn Context\n"
             "- request_id: %" PRIu32 "\n"
             "- source_cap: %s\n"
             "- source_channel: %s\n"
             "- source_chat_id: %s\n"
             "- source_sender_id: %s\n"
             "- source_message_id: %s\n"
             "\n## Behavior Notes\n"
             "%s",
             request->request_id,
             request->source_cap ? request->source_cap : "(unknown)",
             request->source_channel ? request->source_channel : "(unknown)",
             request->source_chat_id ? request->source_chat_id : "(unknown)",
             request->source_sender_id ? request->source_sender_id : "(unknown)",
             request->source_message_id ? request->source_message_id : "(none)",
             k_behavior_note);
    return text;
}

static esp_err_t append_prompt_section(char **prompt,
                                       const char *section_name,
                                       const char *content)
{
    char *grown = NULL;
    size_t current_len;
    size_t extra_len;

    if (!prompt || !*prompt || !section_name || !content || !content[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    current_len = strlen(*prompt);
    extra_len = strlen("\n\n## \n") + strlen(section_name) + strlen(content);
    grown = realloc(*prompt, current_len + extra_len + 1);
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }

    *prompt = grown;
    snprintf((*prompt) + current_len,
             extra_len + 1,
             "\n\n## %s\n%s",
             section_name,
             content);
    return ESP_OK;
}

static esp_err_t append_assistant_tool_calls(cJSON *messages,
                                             const claw_core_llm_response_t *response)
{
    cJSON *assistant = NULL;
    cJSON *tool_calls = NULL;
    size_t i;

    if (!messages || !response) {
        return ESP_ERR_INVALID_ARG;
    }

    assistant = cJSON_CreateObject();
    if (!assistant) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(assistant, "role", "assistant");
    if (response->text && response->text[0]) {
        cJSON_AddStringToObject(assistant, "content", response->text);
    } else {
        cJSON_AddNullToObject(assistant, "content");
    }
    if (response->reasoning_content) {
        cJSON_AddStringToObject(assistant, "reasoning_content", response->reasoning_content);
    } else {
        cJSON_AddStringToObject(assistant, "reasoning_content", "");
    }

    tool_calls = cJSON_CreateArray();
    if (!tool_calls) {
        cJSON_Delete(assistant);
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < response->tool_call_count; i++) {
        cJSON *tool_call = cJSON_CreateObject();
        cJSON *function = cJSON_CreateObject();

        if (!tool_call || !function) {
            cJSON_Delete(tool_call);
            cJSON_Delete(function);
            cJSON_Delete(tool_calls);
            cJSON_Delete(assistant);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(tool_call, "id", response->tool_calls[i].id);
        cJSON_AddStringToObject(tool_call, "type", "function");
        cJSON_AddStringToObject(function, "name", response->tool_calls[i].name);
        cJSON_AddStringToObject(function, "arguments", response->tool_calls[i].arguments_json);
        cJSON_AddItemToObject(tool_call, "function", function);
        cJSON_AddItemToArray(tool_calls, tool_call);
    }

    cJSON_AddItemToObject(assistant, "tool_calls", tool_calls);
    cJSON_AddItemToArray(messages, assistant);
    return ESP_OK;
}

static void claw_core_finish_from_plain_text(uint32_t request_id,
                                             const claw_core_llm_response_t *llm_response,
                                             claw_core_response_t *response)
{
    const char *text = (llm_response && llm_response->text) ? llm_response->text : "";

    response->completion_type = CLAW_CORE_COMPLETION_DONE;
    free(response->text);
    response->text = dup_string(text);
    free(response->error_message);
    response->error_message = NULL;

    ESP_LOGI(TAG, "completion request=%" PRIu32 " status=done raw=%.96s%s",
             request_id,
             log_snippet(text),
             strlen(text) > CLAW_CORE_LOG_SNIPPET_LEN ? "..." : "");
}

static char *claw_core_build_session_failure_trace(const char *error_message,
                                                   const char *tool_summary)
{
    const char *reason = (error_message && error_message[0]) ? error_message : "unknown error";

    if (tool_summary && tool_summary[0]) {
        return dup_printf("Session note: the previous request failed before producing a final answer.\n"
                          "Reason: %s\n%s",
                          reason,
                          tool_summary);
    }

    return dup_printf("Session note: the previous request failed before producing a final answer.\n"
                      "Reason: %s",
                      reason);
}

static esp_err_t append_tool_results_message(cJSON *runtime_messages,
                                             const claw_core_llm_response_t *response,
                                             const claw_core_request_t *request,
                                             char *tool_summary,
                                             size_t tool_summary_size)
{
    size_t i;

    if (!runtime_messages || !response || !request) {
        return ESP_ERR_INVALID_ARG;
    }

    for (i = 0; i < response->tool_call_count; i++) {
        char *tool_output = NULL;
        cJSON *tool_message = NULL;
        esp_err_t err;

        ESP_LOGI(TAG, "tool_call request=%" PRIu32 " name=%s args=%.96s%s",
                 request->request_id,
                 response->tool_calls[i].name ? response->tool_calls[i].name : "(null)",
                 log_snippet(response->tool_calls[i].arguments_json),
                 response->tool_calls[i].arguments_json &&
                 strlen(response->tool_calls[i].arguments_json) > CLAW_CORE_LOG_SNIPPET_LEN ?
                 "..." : "");

        err = claw_core_call_cap(response->tool_calls[i].name,
                                 response->tool_calls[i].arguments_json,
                                 request,
                                 &tool_output);
        if (err != ESP_OK && !tool_output) {
            tool_output = dup_string(esp_err_to_name(err));
        }
        if (!tool_output) {
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(TAG, "tool_result request=%" PRIu32 " name=%s err=%s output=%.96s%s",
                 request->request_id,
                 response->tool_calls[i].name ? response->tool_calls[i].name : "(null)",
                 esp_err_to_name(err),
                 log_snippet(tool_output),
                 strlen(tool_output) > CLAW_CORE_LOG_SNIPPET_LEN ? "..." : "");

        if (tool_summary && tool_summary_size > 0 && response->tool_calls[i].name) {
            esp_err_t summary_err = append_tool_summary_line(tool_summary,
                                                             tool_summary_size,
                                                             response->tool_calls[i].name,
                                                             err == ESP_OK);
            if (summary_err != ESP_OK) {
                ESP_LOGW(TAG, "tool summary truncated for request=%" PRIu32, request->request_id);
            }
        }

        tool_message = cJSON_CreateObject();
        if (!tool_message) {
            free(tool_output);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(tool_message, "role", "tool");
        cJSON_AddStringToObject(tool_message, "tool_call_id", response->tool_calls[i].id);
        cJSON_AddStringToObject(tool_message, "content", tool_output);
        cJSON_AddItemToArray(runtime_messages, tool_message);
        free(tool_output);
    }

    return ESP_OK;
}

static esp_err_t build_iteration_context(const claw_core_request_item_t *request,
                                         const cJSON *runtime_messages,
                                         char **out_system_prompt,
                                         cJSON **out_messages,
                                         char **out_tools_json,
                                         char *obs_providers_csv,
                                         size_t obs_providers_csv_size)
{
    char *system_prompt = NULL;
    char *turn_prompt = NULL;
    cJSON *messages = NULL;
    cJSON *tools = NULL;
    size_t i;
    esp_err_t err = ESP_OK;

    if (!request || !out_system_prompt || !out_messages || !out_tools_json) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_system_prompt = NULL;
    *out_messages = NULL;
    *out_tools_json = NULL;

    system_prompt = dup_string(s_core->system_prompt);
    messages = cJSON_CreateArray();
    tools = cJSON_CreateArray();
    if (!system_prompt || !messages || !tools) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    for (i = 0; i < s_core->context_provider_count; i++) {
        claw_core_context_t context = {0};
        const claw_core_context_provider_t *provider = &s_core->context_providers[i];
        size_t context_len;

        err = provider->collect(&request->view, &context, provider->user_ctx);
        if (err == ESP_ERR_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "context provider collect failed request=%" PRIu32
                     " provider=%s err=%s",
                     request->view.request_id,
                     provider->name,
                     esp_err_to_name(err));
            goto cleanup;
        }
        if (!context.content || !context.content[0]) {
            ESP_LOGW(TAG,
                     "context provider returned empty content request=%" PRIu32
                     " provider=%s",
                     request->view.request_id,
                     provider->name);
            free(context.content);
            err = ESP_FAIL;
            goto cleanup;
        }
        context_len = strlen(context.content);
        ESP_LOGI(TAG,
                 "context_loaded request=%" PRIu32 " provider=%s context_kind=%s context_len=%u",
                 request->view.request_id,
                 provider->name,
                 context_kind_to_string(context.kind),
                 (unsigned)context_len);
        obs_csv_append(obs_providers_csv, obs_providers_csv_size, provider->name, true);

        switch (context.kind) {
        case CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT:
            err = append_prompt_section(&system_prompt, provider->name, context.content);
            break;
        case CLAW_CORE_CONTEXT_KIND_MESSAGES:
            err = append_message_array_json(messages, context.content);
            break;
        case CLAW_CORE_CONTEXT_KIND_TOOLS:
            err = append_tool_array_json(tools, context.content);
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        free(context.content);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    turn_prompt = build_current_turn_prompt(&request->view);
    if (!turn_prompt) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    err = append_prompt_section(&system_prompt, "Core Request", turn_prompt);
    free(turn_prompt);
    turn_prompt = NULL;
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = append_user_message(messages, request->view.user_text);
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (runtime_messages && cJSON_GetArraySize((cJSON *)runtime_messages) > 0) {
        err = append_message_array(messages, runtime_messages);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    *out_tools_json = cJSON_GetArraySize(tools) > 0 ? cJSON_PrintUnformatted(tools) : NULL;
    if (cJSON_GetArraySize(tools) > 0 && !*out_tools_json) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *out_system_prompt = system_prompt;
    *out_messages = messages;
    system_prompt = NULL;
    messages = NULL;
    err = ESP_OK;

cleanup:
    free(turn_prompt);
    free(system_prompt);
    cJSON_Delete(messages);
    cJSON_Delete(tools);
    if (err != ESP_OK) {
        free(*out_tools_json);
        *out_tools_json = NULL;
    }
    return err;
}

static void claw_core_task(void *arg)
{
    (void)arg;

    while (true) {
        claw_core_request_item_t request = {0};
        claw_core_response_item_t response = {0};
        cJSON *runtime_messages = NULL;
        cJSON *messages = NULL;
        char *system_prompt = NULL;
        char *tools_json = NULL;
        char tool_summary[CLAW_CORE_TOOL_SUMMARY_MAX_LEN] = {0};
        claw_core_llm_response_t llm_response = {0};
        uint32_t iteration = 0;
        esp_err_t err = ESP_OK;
        char obs_providers_csv[CLAW_CORE_OBS_CSV_MAX] = {0};
        char obs_tool_calls_csv[CLAW_CORE_OBS_CSV_MAX] = {0};

        if (xQueueReceive(s_core->request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (xSemaphoreTake(s_core->inflight_lock, portMAX_DELAY) == pdTRUE) {
            s_core->inflight_request_id = request.view.request_id;
            s_core->inflight_abort = false;
            xSemaphoreGive(s_core->inflight_lock);
        }
        claw_llm_http_arm_abort(&s_core->inflight_abort);

        response.view.request_id = request.view.request_id;
        response.view.status = CLAW_CORE_RESPONSE_STATUS_ERROR;
        response.view.completion_type = CLAW_CORE_COMPLETION_DONE;
        response.view.target_channel = dup_string(request.view.target_channel);
        response.view.target_chat_id = dup_string(request.view.target_chat_id);
        if ((request.view.target_channel && request.view.target_channel[0] &&
                !response.view.target_channel) ||
                (request.view.target_chat_id && request.view.target_chat_id[0] &&
                 !response.view.target_chat_id)) {
            response.view.error_message = dup_string("Failed to allocate response target");
            goto finish_request;
        }
        if (s_core->on_request_start) {
            err = s_core->on_request_start(&request.view, s_core->on_request_start_user_ctx);
            if (err != ESP_OK) {
                ESP_LOGW(TAG,
                         "request_start request=%" PRIu32 " failed: %s",
                         request.view.request_id,
                         esp_err_to_name(err));
            }
        }

        runtime_messages = cJSON_CreateArray();
        if (!runtime_messages) {
            response.view.error_message = dup_string("Failed to allocate runtime messages");
            goto finish_request;
        }

        while (true) {
            claw_core_llm_response_free(&llm_response);
            free(system_prompt);
            free(tools_json);
            cJSON_Delete(messages);
            system_prompt = NULL;
            tools_json = NULL;
            messages = NULL;

            err = build_iteration_context(&request,
                                          runtime_messages,
                                          &system_prompt,
                                          &messages,
                                          &tools_json,
                                          obs_providers_csv,
                                          sizeof(obs_providers_csv));
            if (err != ESP_OK) {
                response.view.error_message = dup_string(esp_err_to_name(err));
                goto finish_request;
            }

            err = claw_core_llm_chat_messages(system_prompt,
                                              messages,
                                              tools_json,
                                              &llm_response,
                                              &response.view.error_message);
            if (err != ESP_OK) {
                goto finish_request;
            }

            if (llm_response.tool_call_count == 0) {
                publish_stage_note_for_round(&request.view, iteration);
                claw_core_finish_from_plain_text(request.view.request_id,
                                                 &llm_response,
                                                 &response.view);
                err = ESP_OK;
                break;
            }

            log_tool_call_names(request.view.request_id, &llm_response);
            publish_stage_tool_calls(&request.view, &llm_response, iteration);
            for (size_t tc = 0; tc < llm_response.tool_call_count; tc++) {
                obs_csv_append(obs_tool_calls_csv,
                               sizeof(obs_tool_calls_csv),
                               llm_response.tool_calls[tc].name,
                               false);
            }

            err = append_assistant_tool_calls(runtime_messages, &llm_response);
            if (err != ESP_OK) {
                response.view.error_message = dup_string(esp_err_to_name(err));
                goto finish_request;
            }

            err = append_tool_results_message(runtime_messages,
                                              &llm_response,
                                              &request.view,
                                              tool_summary,
                                              sizeof(tool_summary));
            if (err != ESP_OK) {
                response.view.error_message = dup_string(esp_err_to_name(err));
                goto finish_request;
            }

            iteration++;
            if (iteration >= s_core->max_tool_iterations) {
                response.view.error_message = dup_string("cap tool iteration limit reached");
                err = ESP_ERR_INVALID_STATE;
                goto finish_request;
            }
        }

        if (err == ESP_OK && response.view.text) {
            response.view.status = CLAW_CORE_RESPONSE_STATUS_OK;
            if (response.view.text[0] &&
                    s_core->append_session_turn &&
                    request.view.session_id && request.view.session_id[0]) {
                err = s_core->append_session_turn(request.view.session_id,
                                                 request.view.user_text,
                                                 response.view.text,
                                                 s_core->append_session_turn_user_ctx);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "append_session_turn failed: %s", esp_err_to_name(err));
                }
            }
            if (s_core->completion_observer_count > 0) {
                claw_core_completion_summary_t summary = {
                    .request_id = request.view.request_id,
                    .session_id = request.view.session_id,
                    .final_text = response.view.text,
                    .context_providers_csv = obs_providers_csv,
                    .tool_calls_csv = obs_tool_calls_csv,
                };
                for (size_t i = 0; i < s_core->completion_observer_count; i++) {
                    s_core->completion_observers[i].fn(&summary,
                                                     s_core->completion_observers[i].user_ctx);
                }
            }
        } else if (!response.view.error_message) {
            response.view.error_message = dup_string(esp_err_to_name(err));
        }

finish_request:
        claw_llm_http_disarm_abort();
        if (xSemaphoreTake(s_core->inflight_lock, portMAX_DELAY) == pdTRUE) {
            bool was_cancelled = s_core->inflight_abort;
            s_core->inflight_request_id = 0;
            s_core->inflight_abort = false;
            xSemaphoreGive(s_core->inflight_lock);
            if (was_cancelled && err != ESP_OK && response.view.error_message) {
                /* Replace the generic transport error with a clearer one. */
                free(response.view.error_message);
                response.view.error_message = dup_string("request cancelled");
            }
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "request=%" PRIu32 " failed: %s",
                     request.view.request_id,
                     response.view.error_message ? response.view.error_message : esp_err_to_name(err));
            if (s_core->append_session_turn &&
                    request.view.session_id && request.view.session_id[0] &&
                    request.view.user_text && request.view.user_text[0]) {
                char *failure_trace = claw_core_build_session_failure_trace(response.view.error_message,
                                                                            tool_summary);

                if (!failure_trace) {
                    ESP_LOGW(TAG, "append_session_turn skipped for failed request=%" PRIu32 ": no memory",
                             request.view.request_id);
                } else {
                    esp_err_t append_err = s_core->append_session_turn(request.view.session_id,
                                                                       request.view.user_text,
                                                                       failure_trace,
                                                                       s_core->append_session_turn_user_ctx);
                    if (append_err != ESP_OK) {
                        ESP_LOGW(TAG, "append_session_turn failed for failed request=%" PRIu32 ": %s",
                                 request.view.request_id,
                                 esp_err_to_name(append_err));
                    }
                    free(failure_trace);
                }
            }
        }
        publish_out_message_if_requested(&request, &response);
        if (request.view.flags & CLAW_CORE_REQUEST_FLAG_SKIP_RESPONSE_QUEUE) {
            free_response_item(&response);
        } else if (push_response(&response) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enqueue response for request_id=%" PRIu32, request.view.request_id);
            free_response_item(&response);
        }

        claw_core_llm_response_free(&llm_response);
        cJSON_Delete(runtime_messages);
        cJSON_Delete(messages);
        free(system_prompt);
        free(tools_json);
        free_request_item(&request);
    }
}

esp_err_t claw_core_init(const claw_core_config_t *config)
{
    claw_core_llm_config_t llm_config = {0};
    char *llm_error = NULL;
    esp_err_t err;
    uint32_t request_queue_len;
    uint32_t response_queue_len;

    if (!config || !config->system_prompt || !config->api_key || !config->model ||
            (!(config->profile && config->profile[0]) && !(config->provider && config->provider[0]))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_core && s_core->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_core = calloc(1, sizeof(*s_core));
    if (!s_core) {
        return ESP_ERR_NO_MEM;
    }
    claw_core_check_timezone();

    s_core->system_prompt = dup_string(config->system_prompt);
    if (!s_core->system_prompt) {
        claw_core_free_state_storage();
        return ESP_ERR_NO_MEM;
    }
    s_core->append_session_turn = config->append_session_turn;
    s_core->append_session_turn_user_ctx = config->append_session_turn_user_ctx;
    s_core->on_request_start = config->on_request_start;
    s_core->on_request_start_user_ctx = config->on_request_start_user_ctx;
    s_core->collect_stage_note = config->collect_stage_note;
    s_core->collect_stage_note_user_ctx = config->collect_stage_note_user_ctx;
    s_core->call_cap = config->call_cap;
    s_core->cap_user_ctx = config->cap_user_ctx;

    request_queue_len = config->request_queue_len ? config->request_queue_len : CLAW_CORE_DEFAULT_REQUEST_Q;
    response_queue_len = config->response_queue_len ? config->response_queue_len : CLAW_CORE_DEFAULT_RESPONSE_Q;
    s_core->task_stack_size = config->task_stack_size ? config->task_stack_size : CLAW_CORE_DEFAULT_STACK_SIZE;
    s_core->task_priority = config->task_priority ? config->task_priority : CLAW_CORE_DEFAULT_PRIORITY;
    s_core->task_core = config->task_core;
    s_core->max_tool_iterations = config->max_tool_iterations ?
                                 config->max_tool_iterations : CLAW_CORE_DEFAULT_TOOL_ITERATIONS;
    s_core->context_provider_capacity = config->max_context_providers;

    if (s_core->context_provider_capacity > 0) {
        s_core->context_providers = calloc(s_core->context_provider_capacity,
                                           sizeof(claw_core_context_provider_t));
        if (!s_core->context_providers) {
            claw_core_reset_runtime();
            return ESP_ERR_NO_MEM;
        }
    }

    s_core->request_queue = xQueueCreate(request_queue_len, sizeof(claw_core_request_item_t));
    s_core->response_queue = xQueueCreate(response_queue_len, sizeof(claw_core_response_item_t));
    s_core->response_lock = xSemaphoreCreateMutex();
    s_core->inflight_lock = xSemaphoreCreateMutex();
    if (!s_core->request_queue || !s_core->response_queue ||
            !s_core->response_lock || !s_core->inflight_lock) {
        free_context_provider_storage();
        free(s_core->system_prompt);
        if (s_core->request_queue) {
            vQueueDelete(s_core->request_queue);
        }
        if (s_core->response_queue) {
            vQueueDelete(s_core->response_queue);
        }
        if (s_core->response_lock) {
            vSemaphoreDelete(s_core->response_lock);
        }
        if (s_core->inflight_lock) {
            vSemaphoreDelete(s_core->inflight_lock);
        }
        claw_core_free_state_storage();
        return ESP_ERR_NO_MEM;
    }

    llm_config.api_key = config->api_key;
    llm_config.backend_type = config->backend_type;
    llm_config.profile = config->profile;
    llm_config.provider = config->provider;
    llm_config.model = config->model;
    llm_config.base_url = config->base_url;
    llm_config.auth_type = config->auth_type;
    llm_config.timeout_ms = config->timeout_ms;
    llm_config.max_tokens = config->max_tokens;
    llm_config.image_max_bytes = config->image_max_bytes;
    err = claw_core_llm_init(&llm_config, &llm_error);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM init failed: %s", llm_error ? llm_error : esp_err_to_name(err));
        free(llm_error);
        claw_core_reset_runtime();
        return err;
    }

    s_core->initialized = true;
    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

esp_err_t claw_core_start(void)
{
    BaseType_t task_result;

    if (!s_core || !s_core->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_core->started) {
        return ESP_OK;
    }

    task_result = claw_task_create(&(claw_task_config_t){
                                        .name = "claw_core",
                                        .stack_size = s_core->task_stack_size,
                                        .priority = s_core->task_priority,
                                        .core_id = s_core->task_core,
                                        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                                    },
                                    claw_core_task,
                                    NULL,
                                    &s_core->task_handle);

    if (task_result != pdPASS) {
        return ESP_FAIL;
    }

    s_core->started = true;
    ESP_LOGI(TAG, "Started worker task");
    return ESP_OK;
}

esp_err_t claw_core_add_context_provider(const claw_core_context_provider_t *provider)
{
    claw_core_context_provider_t *slot = NULL;

    if (!s_core || !s_core->initialized || s_core->started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!provider || !provider->name || !provider->collect) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_core->context_provider_count >= s_core->context_provider_capacity) {
        return ESP_ERR_NO_MEM;
    }

    slot = &s_core->context_providers[s_core->context_provider_count];
    slot->name = dup_string(provider->name);
    if (!slot->name) {
        return ESP_ERR_NO_MEM;
    }
    slot->collect = provider->collect;
    slot->user_ctx = provider->user_ctx;
    s_core->context_provider_count++;
    return ESP_OK;
}

esp_err_t claw_core_add_completion_observer(claw_core_completion_observer_fn observer,
                                            void *user_ctx)
{
    if (!s_core || !s_core->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!observer) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_core->completion_observer_count >= CLAW_CORE_MAX_COMPLETION_OBSERVERS) {
        return ESP_ERR_NO_MEM;
    }
    s_core->completion_observers[s_core->completion_observer_count].fn = observer;
    s_core->completion_observers[s_core->completion_observer_count].user_ctx = user_ctx;
    s_core->completion_observer_count++;
    return ESP_OK;
}

esp_err_t claw_core_call_cap(const char *cap_name,
                             const char *input_json,
                             const claw_core_request_t *request,
                             char **out_output)
{
    if (!s_core || !s_core->initialized || !s_core->call_cap) {
        return ESP_ERR_INVALID_STATE;
    }

    return s_core->call_cap(cap_name,
                           input_json,
                           request,
                           out_output,
                           s_core->cap_user_ctx);
}

esp_err_t claw_core_cancel_request(uint32_t request_id)
{
    bool armed = false;

    if (!s_core || !s_core->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_core->inflight_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_core->inflight_request_id != 0 &&
            (request_id == 0 || s_core->inflight_request_id == request_id)) {
        s_core->inflight_abort = true;
        armed = true;
        ESP_LOGI(TAG, "Cancel armed for in-flight request=%" PRIu32,
                 s_core->inflight_request_id);
    }
    xSemaphoreGive(s_core->inflight_lock);
    return armed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t claw_core_submit(const claw_core_request_t *request, uint32_t timeout_ms)
{
    claw_core_request_item_t item = {0};
    TickType_t ticks;

    if (!s_core || !s_core->started || !request || !request->user_text || request->user_text[0] == '\0') {
        return (s_core && s_core->started) ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }

    item.view.request_id = request->request_id;
    item.view.flags = request->flags;
    item.owned_session_id = dup_string(request->session_id);
    item.owned_user_text = dup_string(request->user_text);
    item.owned_source_channel = dup_string(request->source_channel);
    item.owned_source_chat_id = dup_string(request->source_chat_id);
    item.owned_source_sender_id = dup_string(request->source_sender_id);
    item.owned_source_message_id = dup_string(request->source_message_id);
    item.owned_source_cap = dup_string(request->source_cap);
    item.owned_target_channel = dup_string(request->target_channel);
    item.owned_target_chat_id = dup_string(request->target_chat_id);

    item.view.session_id = item.owned_session_id;
    item.view.user_text = item.owned_user_text;
    item.view.source_channel = item.owned_source_channel;
    item.view.source_chat_id = item.owned_source_chat_id;
    item.view.source_sender_id = item.owned_source_sender_id;
    item.view.source_message_id = item.owned_source_message_id;
    item.view.source_cap = item.owned_source_cap;
    item.view.target_channel = item.owned_target_channel;
    item.view.target_chat_id = item.owned_target_chat_id;

    if ((request->session_id && !item.owned_session_id) ||
            (request->source_channel && !item.owned_source_channel) ||
            (request->source_chat_id && !item.owned_source_chat_id) ||
            (request->source_sender_id && !item.owned_source_sender_id) ||
            (request->source_message_id && !item.owned_source_message_id) ||
            (request->source_cap && !item.owned_source_cap) ||
            (request->target_channel && !item.owned_target_channel) ||
            (request->target_chat_id && !item.owned_target_chat_id) ||
            !item.owned_user_text) {
        free_request_item(&item);
        return ESP_ERR_NO_MEM;
    }

    ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueSend(s_core->request_queue, &item, ticks) != pdTRUE) {
        free_request_item(&item);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t claw_core_receive(claw_core_response_t *response, uint32_t timeout_ms)
{
    return claw_core_receive_for(0, response, timeout_ms);
}

esp_err_t claw_core_receive_for(uint32_t request_id,
                                claw_core_response_t *response,
                                uint32_t timeout_ms)
{
    claw_core_response_item_t item = {0};
    TickType_t start_ticks;
    bool match_any;

    if (!s_core || !s_core->started || !response) {
        return (s_core && s_core->started) ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_core->response_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    start_ticks = xTaskGetTickCount();
    match_any = (request_id == 0);

    if (pop_pending_response(request_id, match_any, &item)) {
        xSemaphoreGive(s_core->response_lock);
        move_response_item(response, &item);
        return ESP_OK;
    }

    while (true) {
        TickType_t wait_ticks;
        TickType_t elapsed = xTaskGetTickCount() - start_ticks;

        if (timeout_ms == UINT32_MAX) {
            wait_ticks = portMAX_DELAY;
        } else {
            TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

            if (elapsed >= timeout_ticks) {
                xSemaphoreGive(s_core->response_lock);
                return ESP_ERR_TIMEOUT;
            }
            wait_ticks = timeout_ticks - elapsed;
        }

        if (xQueueReceive(s_core->response_queue, &item, wait_ticks) != pdTRUE) {
            xSemaphoreGive(s_core->response_lock);
            return ESP_ERR_TIMEOUT;
        }

        if (match_any || item.view.request_id == request_id) {
            xSemaphoreGive(s_core->response_lock);
            move_response_item(response, &item);
            return ESP_OK;
        }

        if (enqueue_pending_response(&item) != ESP_OK) {
            free_response_item(&item);
        }
    }
}

void claw_core_response_free(claw_core_response_t *response)
{
    if (!response) {
        return;
    }

    free(response->target_channel);
    free(response->target_chat_id);
    free(response->text);
    free(response->error_message);
    response->target_channel = NULL;
    response->target_chat_id = NULL;
    response->text = NULL;
    response->error_message = NULL;
}
