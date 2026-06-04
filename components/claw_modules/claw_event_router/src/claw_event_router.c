/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_event_router.h"

#include <errno.h>
#include <inttypes.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include "cJSON.h"
#include "claw_agent_mgr.h"
#include "claw_core.h"
#include "claw_event_publisher.h"
#include "claw_session_mgr.h"
#include "claw_task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "claw_event_router";

#define CLAW_EVENT_ROUTER_DEFAULT_MAX_RULES          32
#define CLAW_EVENT_ROUTER_DEFAULT_MAX_ACTIONS         8
#define CLAW_EVENT_ROUTER_DEFAULT_OUTPUT_SIZE      2048
#define CLAW_EVENT_ROUTER_DEFAULT_QUEUE_LEN          6
#define CLAW_EVENT_ROUTER_DEFAULT_STACK            8192
#define CLAW_EVENT_ROUTER_DEFAULT_PRIO               5
#define CLAW_EVENT_ROUTER_DEFAULT_SUBMIT          1000
#define CLAW_EVENT_ROUTER_ID_SIZE                  64
#define CLAW_EVENT_ROUTER_DESC_SIZE               160
#define CLAW_EVENT_ROUTER_ACK_SIZE                256
#define CLAW_EVENT_ROUTER_FIELD_SIZE               96
#define CLAW_EVENT_ROUTER_cap_SIZE          64
#define CLAW_EVENT_ROUTER_BINDING_SIZE             16
#define CLAW_EVENT_ROUTER_PENDING_TABLE_SIZE        6
_Static_assert(CLAW_EVENT_ROUTER_PENDING_TABLE_SIZE >= CLAW_EVENT_ROUTER_DEFAULT_QUEUE_LEN,
               "pending table must cover the default event queue length");

typedef struct {
    bool used;
    bool cancelled;
    char event_id[48];
    char event_type[32];
    char source_cap[32];
} claw_event_router_pending_t;

typedef struct {
    char channel[24];
    char cap_name[CLAW_EVENT_ROUTER_cap_SIZE];
} claw_event_router_binding_t;

typedef struct {
    bool has_text;
    const char *text;
    const char *rule;
    const char *remainder;
} claw_event_router_text_match_context_t;

typedef struct {
    bool initialized;
    bool started;
    bool stop_requested;
    SemaphoreHandle_t mutex;
    QueueHandle_t event_queue;
    TaskHandle_t task_handle;
    uint32_t next_request_id;
    char rules_path[192];
    size_t max_rules;
    size_t max_actions_per_rule;
    size_t cap_output_size;
    size_t binding_count;
    claw_event_router_binding_t bindings[CLAW_EVENT_ROUTER_BINDING_SIZE];
    claw_event_router_rule_t *rules;
    size_t rule_count;
    claw_event_router_result_t last_result;
    claw_event_router_config_t config;
    claw_event_router_pending_t pending[CLAW_EVENT_ROUTER_PENDING_TABLE_SIZE];
    size_t pending_dropped;
} claw_event_router_runtime_t;

static claw_event_router_runtime_t *s_runtime = NULL;

static cJSON *claw_event_router_rule_to_json(const claw_event_router_rule_t *rule);
static esp_err_t claw_event_router_load_rules_from_file(const char *path,
                                                        claw_event_router_rule_t **out_rules,
                                                        size_t *out_rule_count,
                                                        cJSON **out_root);
static esp_err_t claw_event_router_write_rules_json_file(const char *path, const char *json);
static esp_err_t claw_event_router_commit_rules(cJSON *root,
                                                claw_event_router_rule_t *new_rules,
                                                size_t new_rule_count);
static void claw_event_router_free_rules(claw_event_router_rule_t *rules, size_t rule_count);

static void claw_event_router_init_defaults(claw_event_router_runtime_t *runtime)
{
    if (!runtime) {
        return;
    }

    runtime->max_rules = CLAW_EVENT_ROUTER_DEFAULT_MAX_RULES;
    runtime->max_actions_per_rule = CLAW_EVENT_ROUTER_DEFAULT_MAX_ACTIONS;
    runtime->cap_output_size = CLAW_EVENT_ROUTER_DEFAULT_OUTPUT_SIZE;
    runtime->next_request_id = 1000000;
}

static void claw_event_router_free_runtime(void)
{
    if (!s_runtime) {
        return;
    }

    claw_event_router_free_rules(s_runtime->rules, s_runtime->rule_count);
    if (s_runtime->event_queue) {
        vQueueDelete(s_runtime->event_queue);
    }
    if (s_runtime->mutex) {
        vSemaphoreDelete(s_runtime->mutex);
    }
    free(s_runtime);
    s_runtime = NULL;
}

static const char *claw_event_router_skip_space(const char *value)
{
    while (value && *value && isspace((unsigned char)*value)) {
        value++;
    }
    return value;
}

static char *claw_event_router_dup_unwrapped_json(const char *json)
{
    const char *start = NULL;
    const char *end = NULL;
    size_t len;
    char *copy = NULL;

    if (!json) {
        return NULL;
    }

    start = claw_event_router_skip_space(json);
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }

    if ((end - start) >= 2 &&
        ((start[0] == '\'' && end[-1] == '\'') || (start[0] == '"' && end[-1] == '"'))) {
        start++;
        end--;
    }

    len = (size_t)(end - start);
    copy = calloc(1, len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, start, len);
    return copy;
}

static esp_err_t claw_event_router_ensure_parent_dir(const char *path)
{
    char dir[224];
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
        ESP_LOGE(TAG, "No existing parent dir found for %s", path);
        return ESP_FAIL;
    }

    for (cursor = create_from; *cursor; cursor++) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir failed for %s errno=%d", dir, errno);
            return ESP_FAIL;
        }
        *cursor = '/';
    }

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir failed for %s errno=%d", dir, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const char *claw_event_router_action_kind_to_string(claw_event_router_action_kind_t kind)
{
    switch (kind) {
    case CLAW_EVENT_ROUTER_ACTION_CALL_CAP:
        return "call_cap";
    case CLAW_EVENT_ROUTER_ACTION_RUN_AGENT:
        return "run_agent";
    case CLAW_EVENT_ROUTER_ACTION_RUN_SCRIPT:
        return "run_script";
    case CLAW_EVENT_ROUTER_ACTION_SEND_MESSAGE:
        return "send_message";
    case CLAW_EVENT_ROUTER_ACTION_EMIT_EVENT:
        return "emit_event";
    case CLAW_EVENT_ROUTER_ACTION_DROP:
        return "drop";
    default:
        return "unknown";
    }
}

static const char *claw_event_router_text_match_rule_to_string(claw_event_router_text_match_rule_t rule)
{
    switch (rule) {
    case CLAW_EVENT_ROUTER_TEXT_MATCH_PREFIX:
        return "prefix";
    case CLAW_EVENT_ROUTER_TEXT_MATCH_EXACT:
    default:
        return "exact";
    }
}

static int64_t claw_event_router_now_ms(void)
{
    struct timeval tv = {0};

    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000LL) + (tv.tv_usec / 1000LL);
}

static void claw_event_router_lock(void)
{
    xSemaphoreTakeRecursive(s_runtime->mutex, portMAX_DELAY);
}

static void claw_event_router_unlock(void)
{
    xSemaphoreGiveRecursive(s_runtime->mutex);
}

static int pending_find_slot_locked(const char *event_id)
{
    if (!event_id || !event_id[0]) {
        return -1;
    }
    for (int i = 0; i < (int)CLAW_EVENT_ROUTER_PENDING_TABLE_SIZE; i++) {
        if (s_runtime->pending[i].used &&
                strcmp(s_runtime->pending[i].event_id, event_id) == 0) {
            return i;
        }
    }
    return -1;
}

static int pending_alloc_slot_locked(void)
{
    int oldest = -1;

    for (int i = 0; i < (int)CLAW_EVENT_ROUTER_PENDING_TABLE_SIZE; i++) {
        if (!s_runtime->pending[i].used) {
            return i;
        }
    }
    for (int i = 0; i < (int)CLAW_EVENT_ROUTER_PENDING_TABLE_SIZE; i++) {
        if (!s_runtime->pending[i].cancelled) {
            oldest = i;
            break;
        }
    }
    if (oldest < 0) {
        oldest = 0;
    }
    return oldest;
}

static void pending_track(const claw_event_t *event)
{
    if (!event || !event->event_id[0]) {
        return;
    }
    claw_event_router_lock();
    int slot = pending_find_slot_locked(event->event_id);
    if (slot < 0) {
        slot = pending_alloc_slot_locked();
        if (s_runtime->pending[slot].used) {
            s_runtime->pending_dropped++;
            ESP_LOGW(TAG,
                     "Pending table full, evicting %s to track %s (dropped=%u)",
                     s_runtime->pending[slot].event_id,
                     event->event_id,
                     (unsigned)s_runtime->pending_dropped);
        }
    }
    memset(&s_runtime->pending[slot], 0, sizeof(s_runtime->pending[slot]));
    s_runtime->pending[slot].used = true;
    s_runtime->pending[slot].cancelled = false;
    strlcpy(s_runtime->pending[slot].event_id,
            event->event_id, sizeof(s_runtime->pending[slot].event_id));
    strlcpy(s_runtime->pending[slot].event_type,
            event->event_type, sizeof(s_runtime->pending[slot].event_type));
    strlcpy(s_runtime->pending[slot].source_cap,
            event->source_cap, sizeof(s_runtime->pending[slot].source_cap));
    claw_event_router_unlock();
}

static bool pending_take_for_event_id(const char *event_id)
{
    bool cancelled = false;

    if (!event_id || !event_id[0]) {
        return false;
    }
    claw_event_router_lock();
    int slot = pending_find_slot_locked(event_id);
    if (slot >= 0) {
        cancelled = s_runtime->pending[slot].cancelled;
        memset(&s_runtime->pending[slot], 0, sizeof(s_runtime->pending[slot]));
    }
    claw_event_router_unlock();
    return cancelled;
}

static bool pending_match_filter(const claw_event_router_pending_t *entry,
                                 const char *event_type_filter,
                                 const char *source_cap_filter)
{
    if (event_type_filter && event_type_filter[0] &&
            strcmp(entry->event_type, event_type_filter) != 0) {
        return false;
    }
    if (source_cap_filter && source_cap_filter[0] &&
            strcmp(entry->source_cap, source_cap_filter) != 0) {
        return false;
    }
    return true;
}

static void claw_event_router_trim_copy(char *dst, size_t dst_size, const char *src)
{
    const char *start = src;
    const char *end = NULL;
    size_t len = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    end = start + strlen(start);
    while (end > start &&
            (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }

    len = (size_t)(end - start);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memmove(dst, start, len);
    dst[len] = '\0';
}

static bool claw_event_router_is_ascii_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f';
}

static const char *claw_event_router_skip_ascii_space(const char *value)
{
    if (!value) {
        return "";
    }
    while (*value && claw_event_router_is_ascii_space(*value)) {
        value++;
    }
    return value;
}

static esp_err_t claw_event_router_read_file(const char *path, char **out_buf)
{
    FILE *file = NULL;
    long size = 0;
    char *buf = NULL;

    if (!path || !out_buf) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;

    file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size < 0) {
        fclose(file);
        return ESP_FAIL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    buf = calloc(1, (size_t)size + 1);
    if (!buf) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(buf);
        return ESP_FAIL;
    }

    fclose(file);
    *out_buf = buf;
    return ESP_OK;
}

void claw_event_router_free_rule(claw_event_router_rule_t *rule)
{
    if (!rule) {
        return;
    }

    free(rule->vars_json);
    for (size_t i = 0; i < rule->action_count; i++) {
        free(rule->actions[i].input_json);
    }
    free(rule->actions);
    memset(rule, 0, sizeof(*rule));
}

void claw_event_router_free_rule_list(claw_event_router_rule_t *rules, size_t rule_count)
{
    if (!rules) {
        return;
    }

    for (size_t i = 0; i < rule_count; i++) {
        claw_event_router_free_rule(&rules[i]);
    }
    free(rules);
}

static void claw_event_router_free_rules(claw_event_router_rule_t *rules, size_t rule_count)
{
    claw_event_router_free_rule_list(rules, rule_count);
}

static bool claw_event_router_parse_caller(const char *value, claw_cap_caller_t *out)
{
    if (!out) {
        return false;
    }
    if (!value || !value[0] || strcmp(value, "system") == 0) {
        *out = CLAW_CAP_CALLER_SYSTEM;
        return true;
    }
    if (strcmp(value, "agent") == 0) {
        *out = CLAW_CAP_CALLER_AGENT;
        return true;
    }
    if (strcmp(value, "console") == 0) {
        *out = CLAW_CAP_CALLER_CONSOLE;
        return true;
    }
    return false;
}

static bool claw_event_router_parse_text_match_rule(const cJSON *match,
                                                    claw_event_router_text_match_rule_t *out_rule)
{
    cJSON *item = NULL;
    const char *value = NULL;

    if (!match || !out_rule) {
        return false;
    }

    *out_rule = CLAW_EVENT_ROUTER_TEXT_MATCH_EXACT;
    item = cJSON_GetObjectItem((cJSON *)match, "text_match_rule");
    if (!item) {
        return true;
    }

    value = cJSON_GetStringValue(item);
    if (!value || !value[0] || strcmp(value, "exact") == 0) {
        return true;
    }
    if (strcmp(value, "prefix") == 0) {
        *out_rule = CLAW_EVENT_ROUTER_TEXT_MATCH_PREFIX;
        return true;
    }
    return false;
}

static const char *claw_event_router_json_string_or_empty(const cJSON *obj, const char *field)
{
    const char *value = NULL;

    if (!obj || !field) {
        return "";
    }
    value = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)obj, field));
    return value ? value : "";
}

static const char *claw_event_router_json_string_with_aliases(const cJSON *obj,
                                                              const char *primary,
                                                              const char *fallback)
{
    const char *value = NULL;

    if (!obj) {
        return "";
    }
    if (primary && primary[0]) {
        value = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)obj, primary));
        if (value && value[0]) {
            return value;
        }
    }
    if (fallback && fallback[0]) {
        value = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)obj, fallback));
        if (value && value[0]) {
            return value;
        }
    }
    return "";
}

static bool claw_event_router_parse_session_policy(const char *value,
                                                   claw_session_policy_t *out_policy)
{
    if (!out_policy) {
        return false;
    }
    if (!value || !value[0] || strcmp(value, "chat") == 0) {
        *out_policy = CLAW_SESSION_POLICY_CHAT;
        return true;
    }
    if (strcmp(value, "trigger") == 0) {
        *out_policy = CLAW_SESSION_POLICY_TRIGGER;
        return true;
    }
    if (strcmp(value, "global") == 0) {
        *out_policy = CLAW_SESSION_POLICY_GLOBAL;
        return true;
    }
    if (strcmp(value, "ephemeral") == 0) {
        *out_policy = CLAW_SESSION_POLICY_EPHEMERAL;
        return true;
    }
    if (strcmp(value, "nosave") == 0) {
        *out_policy = CLAW_SESSION_POLICY_NOSAVE;
        return true;
    }
    return false;
}

static esp_err_t claw_event_router_parse_action(const cJSON *item,
                                                claw_event_router_action_t *out_action)
{
    const char *type = NULL;
    const char *cap = NULL;
    const char *caller = NULL;
    cJSON *input = NULL;
    char *input_json = NULL;

    if (!cJSON_IsObject(item) || !out_action) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_action, 0, sizeof(*out_action));
    type = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)item, "type"));
    caller = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)item, "caller"));
    cap = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)item, "cap"));
    input = cJSON_GetObjectItem((cJSON *)item, "input");

    if (!type || !type[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_event_router_parse_caller(caller, &out_action->caller)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(type, "call_cap") == 0) {
        out_action->kind = CLAW_EVENT_ROUTER_ACTION_CALL_CAP;
        if (!cap || !cap[0] || !input || !cJSON_IsObject(input)) {
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(out_action->cap, cap, sizeof(out_action->cap));
    } else if (strcmp(type, "run_agent") == 0) {
        out_action->kind = CLAW_EVENT_ROUTER_ACTION_RUN_AGENT;
        if (!input || !cJSON_IsObject(input)) {
            input = cJSON_CreateObject();
            if (!input) {
                return ESP_ERR_NO_MEM;
            }
            input_json = cJSON_PrintUnformatted(input);
            cJSON_Delete(input);
            if (!input_json) {
                return ESP_ERR_NO_MEM;
            }
            out_action->input_json = input_json;
            out_action->capture_output = true;
            out_action->fail_open = cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)item, "fail_open"));
            return ESP_OK;
        }
    } else if (strcmp(type, "run_script") == 0) {
        out_action->kind = CLAW_EVENT_ROUTER_ACTION_RUN_SCRIPT;
        if (!input || !cJSON_IsObject(input)) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (strcmp(type, "send_message") == 0) {
        out_action->kind = CLAW_EVENT_ROUTER_ACTION_SEND_MESSAGE;
        if (!input || !cJSON_IsObject(input)) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (strcmp(type, "emit_event") == 0) {
        out_action->kind = CLAW_EVENT_ROUTER_ACTION_EMIT_EVENT;
        if (!input || !cJSON_IsObject(input)) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (strcmp(type, "drop") == 0) {
        out_action->kind = CLAW_EVENT_ROUTER_ACTION_DROP;
        input = cJSON_CreateObject();
        if (!input) {
            return ESP_ERR_NO_MEM;
        }
        input_json = cJSON_PrintUnformatted(input);
        cJSON_Delete(input);
        if (!input_json) {
            return ESP_ERR_NO_MEM;
        }
        out_action->input_json = input_json;
        out_action->capture_output = false;
        out_action->fail_open = cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)item, "fail_open"));
        return ESP_OK;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (!input_json) {
        input_json = cJSON_PrintUnformatted(input);
        if (!input_json) {
            return ESP_ERR_NO_MEM;
        }
    }

    out_action->capture_output = !cJSON_IsBool(cJSON_GetObjectItem((cJSON *)item, "capture_output")) ||
                                 cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)item, "capture_output"));
    out_action->fail_open = cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)item, "fail_open"));
    out_action->input_json = input_json;
    return ESP_OK;
}

static esp_err_t claw_event_router_parse_rule(const cJSON *item,
                                              claw_event_router_rule_t *out_rule)
{
    const char *id = NULL;
    const char *description = NULL;
    const char *ack = NULL;
    cJSON *match = NULL;
    cJSON *actions = NULL;
    cJSON *vars = NULL;
    const char *event_type = NULL;
    cJSON *action = NULL;
    size_t action_count = 0;

    if (!cJSON_IsObject(item) || !out_rule) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_rule, 0, sizeof(*out_rule));
    out_rule->enabled = true;

    id = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)item, "id"));
    description = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)item, "description"));
    ack = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)item, "ack"));
    match = cJSON_GetObjectItem((cJSON *)item, "match");
    actions = cJSON_GetObjectItem((cJSON *)item, "actions");
    vars = cJSON_GetObjectItem((cJSON *)item, "vars");

    if (!id || !id[0] || !cJSON_IsObject(match) || !cJSON_IsArray(actions) ||
            cJSON_GetArraySize(actions) <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    event_type = cJSON_GetStringValue(cJSON_GetObjectItem(match, "event_type"));
    if (!event_type || !event_type[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (vars && !cJSON_IsObject(vars)) {
        return ESP_ERR_INVALID_ARG;
    }

    action_count = (size_t)cJSON_GetArraySize(actions);
    if (action_count > s_runtime->max_actions_per_rule) {
        return ESP_ERR_INVALID_SIZE;
    }

    strlcpy(out_rule->id, id, sizeof(out_rule->id));
    strlcpy(out_rule->description, description ? description : "", sizeof(out_rule->description));
    strlcpy(out_rule->ack, ack ? ack : "", sizeof(out_rule->ack));
    strlcpy(out_rule->match.event_type, event_type, sizeof(out_rule->match.event_type));
    out_rule->enabled = !cJSON_IsBool(cJSON_GetObjectItem((cJSON *)item, "enabled")) ||
                        cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)item, "enabled"));
    out_rule->consume_on_match = !cJSON_IsBool(cJSON_GetObjectItem((cJSON *)item, "consume_on_match")) ||
                                 cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)item, "consume_on_match"));

    strlcpy(out_rule->match.event_key,
            claw_event_router_json_string_or_empty(match, "event_key"),
            sizeof(out_rule->match.event_key));
    strlcpy(out_rule->match.source_cap,
            claw_event_router_json_string_or_empty(match, "source_cap"),
            sizeof(out_rule->match.source_cap));
    strlcpy(out_rule->match.channel,
            claw_event_router_json_string_with_aliases(match, "source_channel", "channel"),
            sizeof(out_rule->match.channel));
    strlcpy(out_rule->match.chat_id,
            claw_event_router_json_string_or_empty(match, "chat_id"),
            sizeof(out_rule->match.chat_id));
    strlcpy(out_rule->match.content_type,
            claw_event_router_json_string_or_empty(match, "content_type"),
            sizeof(out_rule->match.content_type));
    strlcpy(out_rule->match.text,
            claw_event_router_json_string_or_empty(match, "text"),
            sizeof(out_rule->match.text));
    if (!claw_event_router_parse_text_match_rule(match, &out_rule->match.text_match_rule)) {
        ESP_LOGE(TAG, "Unsupported text_match_rule in router rule %s", out_rule->id);
        return ESP_ERR_INVALID_ARG;
    }
    if (out_rule->match.text_match_rule != CLAW_EVENT_ROUTER_TEXT_MATCH_EXACT &&
            !out_rule->match.text[0]) {
        ESP_LOGE(TAG, "text_match_rule requires match.text in router rule %s", out_rule->id);
        return ESP_ERR_INVALID_ARG;
    }

    if (vars) {
        out_rule->vars_json = cJSON_PrintUnformatted(vars);
        if (!out_rule->vars_json) {
            return ESP_ERR_NO_MEM;
        }
    }

    out_rule->actions = calloc(action_count, sizeof(*out_rule->actions));
    if (!out_rule->actions) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_ArrayForEach(action, actions) {
        esp_err_t err = claw_event_router_parse_action(action, &out_rule->actions[out_rule->action_count]);
        if (err != ESP_OK) {
            return err;
        }
        out_rule->action_count++;
    }

    return ESP_OK;
}

static esp_err_t claw_event_router_parse_rule_json(const char *rule_json,
                                                   claw_event_router_rule_t *out_rule)
{
    char *normalized_json = NULL;
    cJSON *item = NULL;
    esp_err_t err;

    if (!rule_json || !rule_json[0] || !out_rule) {
        return ESP_ERR_INVALID_ARG;
    }

    normalized_json = claw_event_router_dup_unwrapped_json(rule_json);
    if (!normalized_json) {
        return ESP_ERR_NO_MEM;
    }

    item = cJSON_Parse(normalized_json);
    free(normalized_json);
    if (!cJSON_IsObject(item)) {
        cJSON_Delete(item);
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_event_router_parse_rule(item, out_rule);
    cJSON_Delete(item);
    return err;
}

static cJSON *claw_event_router_action_to_json(const claw_event_router_action_t *action)
{
    cJSON *item = NULL;
    cJSON *input = NULL;

    if (!action) {
        return NULL;
    }

    item = cJSON_CreateObject();
    if (!item) {
        return NULL;
    }

    cJSON_AddStringToObject(item, "type", claw_event_router_action_kind_to_string(action->kind));
    if (action->caller != CLAW_CAP_CALLER_SYSTEM) {
        cJSON_AddStringToObject(item, "caller",
                                action->caller == CLAW_CAP_CALLER_AGENT ? "agent" : "console");
    }
    if (action->cap[0]) {
        cJSON_AddStringToObject(item, "cap", action->cap);
    }
    if (!action->capture_output) {
        cJSON_AddBoolToObject(item, "capture_output", false);
    }
    if (action->fail_open) {
        cJSON_AddBoolToObject(item, "fail_open", true);
    }

    if (action->input_json && action->input_json[0]) {
        input = cJSON_Parse(action->input_json);
        if (!input) {
            cJSON_Delete(item);
            return NULL;
        }
    } else {
        input = cJSON_CreateObject();
        if (!input) {
            cJSON_Delete(item);
            return NULL;
        }
    }
    cJSON_AddItemToObject(item, "input", input);
    return item;
}

static cJSON *claw_event_router_rule_to_json(const claw_event_router_rule_t *rule)
{
    cJSON *item = NULL;
    cJSON *match = NULL;
    cJSON *actions = NULL;
    cJSON *vars = NULL;

    if (!rule) {
        return NULL;
    }

    item = cJSON_CreateObject();
    match = cJSON_CreateObject();
    actions = cJSON_CreateArray();
    if (!item || !match || !actions) {
        cJSON_Delete(item);
        cJSON_Delete(match);
        cJSON_Delete(actions);
        return NULL;
    }

    cJSON_AddStringToObject(item, "id", rule->id);
    if (rule->description[0]) {
        cJSON_AddStringToObject(item, "description", rule->description);
    }
    if (!rule->enabled) {
        cJSON_AddBoolToObject(item, "enabled", false);
    }
    if (!rule->consume_on_match) {
        cJSON_AddBoolToObject(item, "consume_on_match", false);
    }
    if (rule->ack[0]) {
        cJSON_AddStringToObject(item, "ack", rule->ack);
    }
    if (rule->vars_json && rule->vars_json[0]) {
        vars = cJSON_Parse(rule->vars_json);
        if (!cJSON_IsObject(vars)) {
            cJSON_Delete(vars);
            cJSON_Delete(item);
            cJSON_Delete(match);
            cJSON_Delete(actions);
            return NULL;
        }
        cJSON_AddItemToObject(item, "vars", vars);
    }

    cJSON_AddStringToObject(match, "event_type", rule->match.event_type);
    if (rule->match.event_key[0]) {
        cJSON_AddStringToObject(match, "event_key", rule->match.event_key);
    }
    if (rule->match.source_cap[0]) {
        cJSON_AddStringToObject(match, "source_cap", rule->match.source_cap);
    }
    if (rule->match.channel[0]) {
        cJSON_AddStringToObject(match, "source_channel", rule->match.channel);
    }
    if (rule->match.chat_id[0]) {
        cJSON_AddStringToObject(match, "chat_id", rule->match.chat_id);
    }
    if (rule->match.content_type[0]) {
        cJSON_AddStringToObject(match, "content_type", rule->match.content_type);
    }
    if (rule->match.text[0]) {
        cJSON_AddStringToObject(match, "text", rule->match.text);
    }
    if (rule->match.text_match_rule == CLAW_EVENT_ROUTER_TEXT_MATCH_PREFIX) {
        cJSON_AddStringToObject(match, "text_match_rule",
                                claw_event_router_text_match_rule_to_string(rule->match.text_match_rule));
    }

    for (size_t i = 0; i < rule->action_count; i++) {
        cJSON *action = claw_event_router_action_to_json(&rule->actions[i]);
        if (!action) {
            cJSON_Delete(item);
            cJSON_Delete(match);
            cJSON_Delete(actions);
            return NULL;
        }
        cJSON_AddItemToArray(actions, action);
    }

    cJSON_AddItemToObject(item, "match", match);
    cJSON_AddItemToObject(item, "actions", actions);
    return item;
}

static esp_err_t claw_event_router_load_rules_from_root(const cJSON *root,
                                                        claw_event_router_rule_t **out_rules,
                                                        size_t *out_rule_count)
{
    claw_event_router_rule_t *rules = NULL;
    size_t rule_count = 0;

    if (!out_rules || !out_rule_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_rules = NULL;
    *out_rule_count = 0;

    if (!cJSON_IsArray((cJSON *)root)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((size_t)cJSON_GetArraySize((cJSON *)root) > s_runtime->max_rules) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (cJSON_GetArraySize((cJSON *)root) > 0) {
        rules = calloc((size_t)cJSON_GetArraySize((cJSON *)root), sizeof(*rules));
        if (!rules) {
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, (cJSON *)root) {
        esp_err_t err = claw_event_router_parse_rule(item, &rules[rule_count]);
        if (err != ESP_OK) {
            claw_event_router_free_rule_list(rules, (size_t)cJSON_GetArraySize((cJSON *)root));
            return err;
        }
        rule_count++;
    }

    *out_rules = rules;
    *out_rule_count = rule_count;
    return ESP_OK;
}

static esp_err_t claw_event_router_load_rules_from_file(const char *path,
                                                        claw_event_router_rule_t **out_rules,
                                                        size_t *out_rule_count,
                                                        cJSON **out_root)
{
    char *buf = NULL;
    cJSON *root = NULL;
    esp_err_t err;

    if (!path || !out_rules || !out_rule_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_root) {
        *out_root = NULL;
    }

    err = claw_event_router_read_file(path, &buf);
    if (err == ESP_ERR_NOT_FOUND) {
        root = cJSON_CreateArray();
        if (!root) {
            return ESP_ERR_NO_MEM;
        }
    } else if (err != ESP_OK) {
        return err;
    } else {
        root = cJSON_Parse(buf);
        free(buf);
        if (!root) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    err = claw_event_router_load_rules_from_root(root, out_rules, out_rule_count);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    if (out_root) {
        *out_root = root;
    } else {
        cJSON_Delete(root);
    }
    return ESP_OK;
}

static esp_err_t claw_event_router_write_rules_json_file(const char *path, const char *json)
{
    char temp_path[224];
    FILE *file = NULL;
    size_t json_len = 0;

    if (!path || !path[0] || !json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (claw_event_router_ensure_parent_dir(path) != ESP_OK) {
        return ESP_FAIL;
    }

    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    file = fopen(temp_path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open temp rules file %s errno=%d", temp_path, errno);
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    json_len = strlen(json);
    if ((json_len > 0 && fwrite(json, 1, json_len, file) != json_len) || fflush(file) != 0) {
        fclose(file);
        remove(temp_path);
        ESP_LOGE(TAG, "Failed to write temp rules file %s errno=%d", temp_path, errno);
        return ESP_FAIL;
    }
    fclose(file);

    if (remove(path) != 0 && errno != ENOENT) {
        remove(temp_path);
        return ESP_FAIL;
    }
    if (rename(temp_path, path) != 0) {
        remove(temp_path);
        ESP_LOGE(TAG, "Failed to rename %s to %s errno=%d", temp_path, path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t claw_event_router_commit_rules(cJSON *root,
                                                claw_event_router_rule_t *new_rules,
                                                size_t new_rule_count)
{
    char *json = NULL;
    esp_err_t err;

    if (!root) {
        claw_event_router_free_rule_list(new_rules, new_rule_count);
        return ESP_ERR_INVALID_ARG;
    }

    json = cJSON_Print(root);
    if (!json) {
        cJSON_Delete(root);
        claw_event_router_free_rule_list(new_rules, new_rule_count);
        return ESP_ERR_NO_MEM;
    }

    err = claw_event_router_write_rules_json_file(s_runtime->rules_path, json);
    free(json);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        claw_event_router_free_rule_list(new_rules, new_rule_count);
        return err;
    }

    claw_event_router_lock();
    claw_event_router_free_rules(s_runtime->rules, s_runtime->rule_count);
    s_runtime->rules = new_rules;
    s_runtime->rule_count = new_rule_count;
    claw_event_router_unlock();
    return ESP_OK;
}

static const char *claw_event_router_event_key(const claw_event_t *event)
{
    if (!event) {
        return "";
    }
    if (strcmp(event->event_type, "message") == 0) {
        return "text";
    }
    if (event->message_id[0]) {
        return event->message_id;
    }
    return event->event_id;
}

static bool claw_event_router_match_field(const char *expected, const char *actual)
{
    return !expected || !expected[0] || strcmp(expected, actual ? actual : "") == 0;
}

static bool claw_event_router_match_text_exact(const char *expected,
                                               const char *actual,
                                               claw_event_router_text_match_context_t *out_match)
{
    if (!expected || !expected[0]) {
        return true;
    }
    if (strcmp(expected, actual ? actual : "") != 0) {
        return false;
    }
    if (out_match) {
        out_match->has_text = true;
        out_match->text = expected;
        out_match->rule = claw_event_router_text_match_rule_to_string(CLAW_EVENT_ROUTER_TEXT_MATCH_EXACT);
        out_match->remainder = "";
    }
    return true;
}

static bool claw_event_router_match_text_prefix(const char *expected,
                                                const char *actual,
                                                claw_event_router_text_match_context_t *out_match)
{
    const char *trimmed = NULL;
    const char *remainder = NULL;
    size_t expected_len = 0;

    if (!expected || !expected[0]) {
        return true;
    }

    trimmed = claw_event_router_skip_ascii_space(actual);
    expected_len = strlen(expected);
    if (strncmp(trimmed, expected, expected_len) != 0) {
        return false;
    }

    remainder = trimmed + expected_len;
    if (*remainder && !claw_event_router_is_ascii_space(*remainder)) {
        return false;
    }
    remainder = claw_event_router_skip_ascii_space(remainder);

    if (out_match) {
        out_match->has_text = true;
        out_match->text = expected;
        out_match->rule = claw_event_router_text_match_rule_to_string(CLAW_EVENT_ROUTER_TEXT_MATCH_PREFIX);
        out_match->remainder = remainder;
    }
    return true;
}

static bool claw_event_router_match_text(const claw_event_router_match_t *match,
                                         const char *actual,
                                         claw_event_router_text_match_context_t *out_match)
{
    if (out_match) {
        memset(out_match, 0, sizeof(*out_match));
    }
    if (!match) {
        return false;
    }

    switch (match->text_match_rule) {
    case CLAW_EVENT_ROUTER_TEXT_MATCH_PREFIX:
        return claw_event_router_match_text_prefix(match->text, actual, out_match);
    case CLAW_EVENT_ROUTER_TEXT_MATCH_EXACT:
    default:
        return claw_event_router_match_text_exact(match->text, actual, out_match);
    }
}

static bool claw_event_router_rule_matches(const claw_event_router_rule_t *rule,
                                           const claw_event_t *event,
                                           claw_event_router_text_match_context_t *out_match)
{
    return rule && rule->enabled &&
           claw_event_router_match_field(rule->match.event_type, event->event_type) &&
           claw_event_router_match_field(rule->match.event_key, claw_event_router_event_key(event)) &&
           claw_event_router_match_field(rule->match.source_cap, event->source_cap) &&
           claw_event_router_match_field(rule->match.channel, event->source_channel) &&
           claw_event_router_match_field(rule->match.chat_id, event->chat_id) &&
           claw_event_router_match_field(rule->match.content_type, event->content_type) &&
           claw_event_router_match_text(&rule->match, event->text, out_match);
}

static cJSON *claw_event_router_build_event_context(const claw_event_t *event)
{
    cJSON *ctx = NULL;
    cJSON *event_obj = NULL;
    cJSON *payload_obj = NULL;

    if (!event) {
        return NULL;
    }

    ctx = cJSON_CreateObject();
    event_obj = cJSON_CreateObject();
    if (!ctx || !event_obj) {
        cJSON_Delete(ctx);
        cJSON_Delete(event_obj);
        return NULL;
    }

    cJSON_AddStringToObject(event_obj, "event_id", event->event_id);
    cJSON_AddStringToObject(event_obj, "event_type", event->event_type);
    cJSON_AddStringToObject(event_obj, "event_key", claw_event_router_event_key(event));
    cJSON_AddStringToObject(event_obj, "source_cap", event->source_cap);
    cJSON_AddStringToObject(event_obj, "channel", event->source_channel);
    cJSON_AddStringToObject(event_obj, "source_channel", event->source_channel);
    cJSON_AddStringToObject(event_obj, "target_channel", event->target_channel);
    cJSON_AddStringToObject(event_obj, "source_endpoint", event->source_endpoint);
    cJSON_AddStringToObject(event_obj, "target_endpoint", event->target_endpoint);
    cJSON_AddStringToObject(event_obj, "chat_id", event->chat_id);
    cJSON_AddStringToObject(event_obj, "sender_id", event->sender_id);
    cJSON_AddStringToObject(event_obj, "message_id", event->message_id);
    cJSON_AddStringToObject(event_obj, "correlation_id", event->correlation_id);
    cJSON_AddStringToObject(event_obj, "content_type", event->content_type);
    cJSON_AddStringToObject(event_obj, "session_policy",
                            claw_event_session_policy_to_string(event->session_policy));
    cJSON_AddNumberToObject(event_obj, "timestamp_ms", (double)event->timestamp_ms);
    cJSON_AddStringToObject(event_obj, "text", event->text ? event->text : "");

    if (event->payload_json && event->payload_json[0]) {
        payload_obj = cJSON_Parse(event->payload_json);
        if (!cJSON_IsObject(payload_obj)) {
            cJSON_Delete(payload_obj);
            payload_obj = cJSON_CreateObject();
        }
    } else {
        payload_obj = cJSON_CreateObject();
    }
    if (!payload_obj) {
        cJSON_Delete(ctx);
        cJSON_Delete(event_obj);
        return NULL;
    }

    cJSON_AddItemToObject(event_obj, "payload", payload_obj);
    cJSON_AddItemToObject(ctx, "event", event_obj);
    return ctx;
}

static esp_err_t claw_event_router_update_match_context(cJSON *ctx,
                                                        const claw_event_router_text_match_context_t *match_ctx)
{
    cJSON *match_obj = NULL;

    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(ctx, "match");
    if (!match_ctx || !match_ctx->has_text) {
        return ESP_OK;
    }

    match_obj = cJSON_CreateObject();
    if (!match_obj) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(match_obj, "text", match_ctx->text ? match_ctx->text : "");
    cJSON_AddStringToObject(match_obj, "rule", match_ctx->rule ? match_ctx->rule : "");
    cJSON_AddStringToObject(match_obj, "remainder", match_ctx->remainder ? match_ctx->remainder : "");
    cJSON_AddItemToObject(ctx, "match", match_obj);
    return ESP_OK;
}

static char *claw_event_router_lookup_string_dup(const cJSON *ctx,
                                                 const char *path)
{
    char path_buf[128];
    const cJSON *node = ctx;
    char *segment = NULL;
    char num_buf[32];
    const char *value = NULL;

    if (!ctx || !path || !path[0]) {
        return NULL;
    }
    strlcpy(path_buf, path, sizeof(path_buf));
    segment = path_buf;
    while (segment && segment[0]) {
        char *dot = strchr(segment, '.');
        if (dot) {
            *dot = '\0';
        }
        if (!cJSON_IsObject(node)) {
            return NULL;
        }
        node = cJSON_GetObjectItemCaseSensitive((cJSON *)node, segment);
        if (!node) {
            return NULL;
        }
        if (!dot) {
            break;
        }
        segment = dot + 1;
    }

    if (cJSON_IsString(node) && node->valuestring) {
        value = node->valuestring;
    } else if (cJSON_IsNumber(node)) {
        snprintf(num_buf, sizeof(num_buf), "%g", node->valuedouble);
        value = num_buf;
    } else if (cJSON_IsBool(node)) {
        value = cJSON_IsTrue(node) ? "true" : "false";
    } else if (cJSON_IsNull(node)) {
        value = "null";
    } else {
        return NULL;
    }

    return strdup(value);
}

static char *claw_event_router_render_string(const char *template_str, const cJSON *ctx)
{
    size_t in_len = 0;
    size_t out_cap = 0;
    size_t out_len = 0;
    char *out = NULL;

    if (!template_str) {
        return strdup("");
    }

    in_len = strlen(template_str);
    out_cap = in_len + 32;
    out = calloc(1, out_cap);
    if (!out) {
        return NULL;
    }

    for (size_t i = 0; i < in_len;) {
        if (i + 1 < in_len && template_str[i] == '{' && template_str[i + 1] == '{') {
            size_t start = i + 2;
            size_t end = start;
            char key[96];
            char *value = NULL;
            size_t key_len;
            size_t value_len;

            while (end + 1 < in_len &&
                    !(template_str[end] == '}' && template_str[end + 1] == '}')) {
                end++;
            }
            if (end + 1 >= in_len) {
                break;
            }

            key_len = end - start;
            if (key_len >= sizeof(key)) {
                key_len = sizeof(key) - 1;
            }
            memcpy(key, template_str + start, key_len);
            key[key_len] = '\0';
            claw_event_router_trim_copy(key, sizeof(key), key);

            value = claw_event_router_lookup_string_dup(ctx, key);
            if (!value) {
                value = strdup("");
            }
            if (!value) {
                free(out);
                return NULL;
            }
            value_len = strlen(value);
            if (out_len + value_len + 1 > out_cap) {
                char *grown = realloc(out, out_len + value_len + 32);
                if (!grown) {
                    free(value);
                    free(out);
                    return NULL;
                }
                out = grown;
                out_cap = out_len + value_len + 32;
            }
            memcpy(out + out_len, value, value_len);
            free(value);
            out_len += value_len;
            out[out_len] = '\0';
            i = end + 2;
            continue;
        }

        if (out_len + 2 > out_cap) {
            char *grown = realloc(out, out_cap + 32);
            if (!grown) {
                free(out);
                return NULL;
            }
            out = grown;
            out_cap += 32;
        }
        out[out_len++] = template_str[i++];
        out[out_len] = '\0';
    }

    return out;
}

static cJSON *claw_event_router_render_json(const cJSON *input, const cJSON *ctx)
{
    cJSON *out = NULL;
    cJSON *child = NULL;

    if (!input) {
        return cJSON_CreateNull();
    }
    if (cJSON_IsObject(input)) {
        out = cJSON_CreateObject();
        if (!out) {
            return NULL;
        }
        cJSON_ArrayForEach(child, input) {
            cJSON *rendered = claw_event_router_render_json(child, ctx);
            if (!rendered) {
                cJSON_Delete(out);
                return NULL;
            }
            cJSON_AddItemToObject(out, child->string, rendered);
        }
        return out;
    }
    if (cJSON_IsArray(input)) {
        out = cJSON_CreateArray();
        if (!out) {
            return NULL;
        }
        cJSON_ArrayForEach(child, input) {
            cJSON *rendered = claw_event_router_render_json(child, ctx);
            if (!rendered) {
                cJSON_Delete(out);
                return NULL;
            }
            cJSON_AddItemToArray(out, rendered);
        }
        return out;
    }
    if (cJSON_IsString(input)) {
        char *rendered = claw_event_router_render_string(input->valuestring, ctx);
        if (!rendered) {
            return NULL;
        }
        out = cJSON_CreateString(rendered);
        free(rendered);
        return out;
    }
    return cJSON_Duplicate((cJSON *)input, 1);
}

static esp_err_t claw_event_router_prepare_session_id(const claw_event_t *event,
                                                              char *buf,
                                                              size_t buf_size,
                                                              size_t *out_len)
{
    claw_session_build_context_t ctx = {0};

    if (!event || !buf || buf_size == 0 || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx.agent_id = 0;
    ctx.session_policy = event->session_policy;
    ctx.source_cap = event->source_cap;
    ctx.event_type = event->event_type;
    ctx.source_channel = event->source_channel;
    ctx.chat_id = event->chat_id;
    ctx.message_id = event->message_id;
    ctx.event_id = event->event_id;

    return claw_session_mgr_build_session_id(&ctx, buf, buf_size, out_len);
}

static esp_err_t claw_event_router_default_outbound_resolver(const claw_event_t *event,
                                                             const char *target_channel,
                                                             const char *target_endpoint,
                                                             char *cap_name,
                                                             size_t cap_name_size)
{
    size_t i;

    (void)event;
    (void)target_endpoint;

    if (!cap_name || cap_name_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cap_name[0] = '\0';

    claw_event_router_lock();
    for (i = 0; i < s_runtime->binding_count; i++) {
        if (strcmp(s_runtime->bindings[i].channel, target_channel ? target_channel : "") == 0) {
            strlcpy(cap_name,
                    s_runtime->bindings[i].cap_name,
                    cap_name_size);
            claw_event_router_unlock();
            return ESP_OK;
        }
    }
    claw_event_router_unlock();
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t claw_event_router_resolve_outbound_cap(const claw_event_t *event,
                                                        const char *target_channel,
                                                        const char *target_endpoint,
                                                        char *cap_name,
                                                        size_t cap_name_size)
{
    if (s_runtime->config.outbound_resolver) {
        esp_err_t err = s_runtime->config.outbound_resolver(event,
                                                           target_channel,
                                                           target_endpoint,
                                                           cap_name,
                                                           cap_name_size,
                                                           s_runtime->config.outbound_resolver_user_ctx);
        if (err != ESP_ERR_NOT_FOUND) {
            return err;
        }
    }

    return claw_event_router_default_outbound_resolver(event,
                                                       target_channel,
                                                       target_endpoint,
                                                       cap_name,
                                                       cap_name_size);
}

static void claw_event_router_update_last_output(cJSON *ctx,
                                                 const char *kind,
                                                 const char *target,
                                                 const char *status,
                                                 const char *output)
{
    cJSON *last_obj = NULL;

    last_obj = cJSON_GetObjectItemCaseSensitive(ctx, "last");
    if (!last_obj) {
        last_obj = cJSON_CreateObject();
        if (!last_obj) {
            return;
        }
        cJSON_AddItemToObject(ctx, "last", last_obj);
    }

    cJSON_DeleteItemFromObjectCaseSensitive(last_obj, "kind");
    cJSON_DeleteItemFromObjectCaseSensitive(last_obj, "target");
    cJSON_DeleteItemFromObjectCaseSensitive(last_obj, "status");
    cJSON_DeleteItemFromObjectCaseSensitive(last_obj, "output");
    cJSON_AddStringToObject(last_obj, "kind", kind ? kind : "");
    cJSON_AddStringToObject(last_obj, "target", target ? target : "");
    cJSON_AddStringToObject(last_obj, "status", status ? status : "");
    cJSON_AddStringToObject(last_obj, "output", output ? output : "");
}

static const char *claw_event_router_get_ctx_string(const cJSON *ctx,
                                                    const char *group,
                                                    const char *field)
{
    const cJSON *obj = NULL;
    const cJSON *item = NULL;

    if (!cJSON_IsObject((cJSON *)ctx) || !group || !field) {
        return NULL;
    }
    obj = cJSON_GetObjectItemCaseSensitive((cJSON *)ctx, group);
    if (!cJSON_IsObject((cJSON *)obj)) {
        return NULL;
    }
    item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, field);
    if (!cJSON_IsString((cJSON *)item) || !item->valuestring || !item->valuestring[0]) {
        return NULL;
    }
    return item->valuestring;
}

static esp_err_t claw_event_router_execute_cap_action(
    const claw_event_router_rule_t *rule,
    const claw_event_router_action_t *action,
    const claw_event_t *event,
    cJSON *ctx,
    claw_event_router_result_t *result)
{
    cJSON *input_root = NULL;
    cJSON *rendered_input = NULL;
    char *input_json = NULL;
    char *output = NULL;
    char session_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    size_t session_id_len = 0;
    claw_cap_call_context_t call_ctx = {0};
    esp_err_t err = ESP_OK;

    input_root = cJSON_Parse(action->input_json);
    if (!cJSON_IsObject(input_root)) {
        cJSON_Delete(input_root);
        return ESP_ERR_INVALID_ARG;
    }
    rendered_input = claw_event_router_render_json(input_root, ctx);
    cJSON_Delete(input_root);
    if (!rendered_input) {
        return ESP_ERR_NO_MEM;
    }
    input_json = cJSON_PrintUnformatted(rendered_input);
    cJSON_Delete(rendered_input);
    if (!input_json) {
        return ESP_ERR_NO_MEM;
    }

    output = calloc(1, s_runtime->cap_output_size);
    if (!output) {
        free(input_json);
        return ESP_ERR_NO_MEM;
    }

    call_ctx.channel = event->source_channel;
    call_ctx.chat_id = event->chat_id;
    call_ctx.target_channel = event->target_channel[0] ? event->target_channel : event->source_channel;
    call_ctx.target_chat_id = event->target_endpoint[0] ? event->target_endpoint : event->chat_id;
    err = claw_event_router_prepare_session_id(event, session_id, sizeof(session_id), &session_id_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to build cap action session id: %s", esp_err_to_name(err));
        free(input_json);
        free(output);
        return err;
    }
    if (session_id_len > 0) {
        call_ctx.session_id = session_id;
    }
    call_ctx.source_cap = "claw_event_router";
    call_ctx.correlation_id = event->correlation_id[0] ? event->correlation_id : event->message_id;
    call_ctx.caller = action->caller;

    err = claw_cap_call(action->cap,
                        input_json,
                        &call_ctx,
                        output,
                        s_runtime->cap_output_size);
    free(input_json);

    claw_event_router_update_last_output(ctx,
                                         "cap",
                                         action->cap,
                                         err == ESP_OK ? "ok" : "error",
                                         action->capture_output ? output : "");

    if (result) {
        result->action_count++;
        if (err != ESP_OK) {
            result->failed_actions++;
            result->last_error = err;
        }
    }

    free(output);
    if (err != ESP_OK && !action->fail_open) {
        ESP_LOGW(TAG, "Rule %s action %s failed: %s",
                 rule->id,
                 action->cap,
                 esp_err_to_name(err));
    }
    return err;
}

static esp_err_t claw_event_router_execute_agent_action(
    const claw_event_router_rule_t *rule,
    const claw_event_router_action_t *action,
    const claw_event_t *event,
    cJSON *ctx,
    claw_event_router_result_t *result)
{
    cJSON *input_root = NULL;
    cJSON *rendered_input = NULL;
    const char *text = NULL;
    const char *target_channel = NULL;
    const char *target_chat_id = NULL;
    const char *session_policy = NULL;
    claw_event_t agent_event = {0};
    claw_agent_mgr_root_input_t agent_input = {0};
    char submit_output[32] = {0};
    esp_err_t err;

    input_root = cJSON_Parse(action->input_json);
    if (!cJSON_IsObject(input_root)) {
        cJSON_Delete(input_root);
        return ESP_ERR_INVALID_ARG;
    }
    rendered_input = claw_event_router_render_json(input_root, ctx);
    cJSON_Delete(input_root);
    if (!rendered_input) {
        return ESP_ERR_NO_MEM;
    }

    text = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "text"));
    target_channel = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "target_channel"));
    target_chat_id = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "target_chat_id"));
    session_policy = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "session_policy"));

    agent_event = *event;
    if (session_policy && session_policy[0]) {
        claw_event_router_parse_session_policy(session_policy, &agent_event.session_policy);
    }

    agent_input.session_policy = agent_event.session_policy;
    agent_input.flags = CLAW_CORE_REQUEST_FLAG_PUBLISH_OUT_MESSAGE |
                        CLAW_CORE_REQUEST_FLAG_PUBLISH_STAGE_MESSAGE |
                        CLAW_CORE_REQUEST_FLAG_SKIP_RESPONSE_QUEUE |
                        CLAW_CORE_REQUEST_FLAG_USER_INTERRUPT;
    agent_input.request_id = s_runtime->next_request_id++;
    agent_input.user_text = (text && text[0]) ? text : (event->text ? event->text : "");
    agent_input.source_cap = event->source_cap;
    agent_input.event_type = event->event_type;
    agent_input.source_channel = event->source_channel;
    agent_input.source_chat_id = event->chat_id;
    agent_input.source_sender_id = event->sender_id;
    agent_input.source_message_id = event->message_id;
    agent_input.event_id = event->event_id;
    agent_input.target_channel = (target_channel && target_channel[0]) ? target_channel : event->source_channel;
    agent_input.target_chat_id = (target_chat_id && target_chat_id[0]) ? target_chat_id : event->chat_id;

    err = claw_agent_mgr_submit_root(&agent_input,
                                     s_runtime->config.agent_submit_timeout_ms);

    if (err == ESP_OK) {
        snprintf(submit_output, sizeof(submit_output), "request_id=%" PRIu32, agent_input.request_id);
        claw_event_router_update_last_output(ctx,
                                             "agent",
                                             agent_input.target_channel,
                                             "submitted",
                                             submit_output);
    } else {
        claw_event_router_update_last_output(ctx, "agent", agent_input.target_channel, "error",
                                             esp_err_to_name(err));
    }

    if (result) {
        result->action_count++;
        if (err != ESP_OK) {
            result->failed_actions++;
            result->last_error = err;
        }
    }

    if (err != ESP_OK && !action->fail_open) {
        ESP_LOGW(TAG, "Rule %s agent action failed: %s", rule->id, esp_err_to_name(err));
    }

    cJSON_Delete(rendered_input);
    return err;
}

static esp_err_t claw_event_router_execute_send_message_action(
    const claw_event_router_rule_t *rule,
    const claw_event_router_action_t *action,
    const claw_event_t *event,
    cJSON *ctx,
    claw_event_router_result_t *result)
{
    cJSON *input_root = NULL;
    cJSON *rendered_input = NULL;
    const char *channel = NULL;
    const char *chat_id = NULL;
    const char *message = NULL;
    char cap_name[CLAW_EVENT_ROUTER_cap_SIZE] = {0};
    char output[256] = {0};
    cJSON *payload_root = NULL;
    char *payload = NULL;
    claw_cap_call_context_t call_ctx = {0};
    esp_err_t err;

    input_root = cJSON_Parse(action->input_json);
    if (!cJSON_IsObject(input_root)) {
        cJSON_Delete(input_root);
        return ESP_ERR_INVALID_ARG;
    }
    rendered_input = claw_event_router_render_json(input_root, ctx);
    cJSON_Delete(input_root);
    if (!rendered_input) {
        return ESP_ERR_NO_MEM;
    }

    channel = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "channel"));
    chat_id = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "chat_id"));
    message = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "message"));
    ESP_LOGD(TAG,
             "send_message rendered channel=%s chat_id=%s message_len=%u",
             channel ? channel : "(null)",
             chat_id ? chat_id : "(null)",
             (unsigned int)(message ? strlen(message) : 0));
    if (!channel || !channel[0]) {
        channel = event->target_channel[0] ? event->target_channel : event->source_channel;
    }
    if (!chat_id || !chat_id[0]) {
        chat_id = event->target_endpoint[0] ? event->target_endpoint : event->chat_id;
    }
    if (!message || !message[0]) {
        message = claw_event_router_get_ctx_string(ctx, "last", "output");
        ESP_LOGD(TAG,
                 "send_message fallback last.output message_len=%u",
                 (unsigned int)(message ? strlen(message) : 0));
    }
    if (!message || !message[0]) {
        ESP_LOGW(TAG,
                 "send_message dropped: empty message rule=%s channel=%s chat_id=%s",
                 rule ? rule->id : "-",
                 channel ? channel : "(null)",
                 chat_id ? chat_id : "(null)");
        cJSON_Delete(rendered_input);
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_event_router_resolve_outbound_cap(event,
                                                 channel,
                                                 chat_id,
                                                 cap_name,
                                                 sizeof(cap_name));
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "send_message resolve failed channel=%s chat_id=%s err=%s",
                 channel ? channel : "(null)",
                 chat_id ? chat_id : "(null)",
                 esp_err_to_name(err));
        cJSON_Delete(rendered_input);
        return err;
    }
    ESP_LOGD(TAG, "send_message resolved cap=%s", cap_name);

    payload_root = cJSON_CreateObject();
    if (!payload_root) {
        cJSON_Delete(rendered_input);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(payload_root, "chat_id", chat_id);
    cJSON_AddStringToObject(payload_root, "message", message);
    cJSON_AddStringToObject(payload_root, "event_type", event->event_type);
    payload = cJSON_PrintUnformatted(payload_root);
    cJSON_Delete(payload_root);
    if (!payload) {
        cJSON_Delete(rendered_input);
        return ESP_ERR_NO_MEM;
    }
    call_ctx.channel = channel;
    call_ctx.chat_id = chat_id;
    call_ctx.target_channel = channel;
    call_ctx.target_chat_id = chat_id;
    call_ctx.source_cap = "claw_event_router";
    call_ctx.caller = CLAW_CAP_CALLER_SYSTEM;
    err = claw_cap_call(cap_name, payload, &call_ctx, output, sizeof(output));
    ESP_LOGI(TAG,
             "send_message cap=%s err=%s output=%s",
             cap_name,
             esp_err_to_name(err),
             output[0] ? output : "-");
    free(payload);
    claw_event_router_update_last_output(ctx,
                                         "send_message",
                                         cap_name,
                                         err == ESP_OK ? "ok" : "error",
                                         err == ESP_OK ? message : output);

    if (result) {
        result->action_count++;
        if (err != ESP_OK) {
            result->failed_actions++;
            result->last_error = err;
        }
    }
    if (err != ESP_OK && !action->fail_open) {
        ESP_LOGW(TAG, "Rule %s send_message via %s failed: %s",
                 rule->id,
                 cap_name,
                 esp_err_to_name(err));
    }

    cJSON_Delete(rendered_input);
    return err;
}

static esp_err_t claw_event_router_execute_script_action(
    const claw_event_router_rule_t *rule,
    const claw_event_router_action_t *action,
    const claw_event_t *event,
    cJSON *ctx,
    claw_event_router_result_t *result)
{
    claw_event_router_action_t local = *action;
    char cap_name[CLAW_EVENT_ROUTER_cap_SIZE];
    cJSON *input_root = cJSON_Parse(action->input_json);
    const char *async_value = NULL;

    if (!cJSON_IsObject(input_root)) {
        cJSON_Delete(input_root);
        return ESP_ERR_INVALID_ARG;
    }
    async_value = cJSON_GetStringValue(cJSON_GetObjectItem(input_root, "cap"));
    (void)async_value;
    strlcpy(cap_name,
            cJSON_IsTrue(cJSON_GetObjectItem(input_root, "async")) ?
            "lua_run_script_async" : "lua_run_script",
            sizeof(cap_name));
    cJSON_Delete(input_root);
    strlcpy(local.cap, cap_name, sizeof(local.cap));
    local.kind = CLAW_EVENT_ROUTER_ACTION_CALL_CAP;
    return claw_event_router_execute_cap_action(rule, &local, event, ctx, result);
}

static esp_err_t claw_event_router_execute_emit_event_action(
    const claw_event_router_action_t *action,
    cJSON *ctx,
    claw_event_router_result_t *result)
{
    cJSON *input_root = NULL;
    cJSON *rendered_input = NULL;
    claw_event_t event = {0};
    const char *value = NULL;
    esp_err_t err;

    input_root = cJSON_Parse(action->input_json);
    if (!cJSON_IsObject(input_root)) {
        cJSON_Delete(input_root);
        return ESP_ERR_INVALID_ARG;
    }
    rendered_input = claw_event_router_render_json(input_root, ctx);
    cJSON_Delete(input_root);
    if (!rendered_input) {
        return ESP_ERR_NO_MEM;
    }

    value = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "source_cap"));
    strlcpy(event.source_cap, value ? value : "claw_event_router", sizeof(event.source_cap));
    value = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "event_type"));
    strlcpy(event.event_type, value ? value : "trigger", sizeof(event.event_type));
    value = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "source_channel"));
    strlcpy(event.source_channel, value ? value : "", sizeof(event.source_channel));
    value = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "chat_id"));
    strlcpy(event.chat_id, value ? value : "", sizeof(event.chat_id));
    value = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "message_id"));
    strlcpy(event.message_id, value ? value : "", sizeof(event.message_id));
    value = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "content_type"));
    strlcpy(event.content_type, value ? value : "trigger", sizeof(event.content_type));
    value = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "text"));
    event.text = (char *)(value ? value : "");
    value = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "payload_json"));
    event.payload_json = (char *)(value ? value : "{}");
    event.timestamp_ms = claw_event_router_now_ms();
    value = cJSON_GetStringValue(cJSON_GetObjectItem(rendered_input, "session_policy"));
    if (!claw_event_router_parse_session_policy(value, &event.session_policy)) {
        event.session_policy = CLAW_SESSION_POLICY_TRIGGER;
    }
    snprintf(event.event_id, sizeof(event.event_id), "evt-%" PRId64, event.timestamp_ms);

    err = claw_event_router_publish(&event);
    if (result) {
        result->action_count++;
        if (err != ESP_OK) {
            result->failed_actions++;
            result->last_error = err;
        }
    }

    cJSON_Delete(rendered_input);
    return err;
}

static esp_err_t claw_event_router_execute_action(const claw_event_router_rule_t *rule,
                                                  const claw_event_router_action_t *action,
                                                  const claw_event_t *event,
                                                  cJSON *ctx,
                                                  claw_event_router_result_t *result)
{
    ESP_LOGD(TAG,
             "event=%s rule=%s action=%s start",
             event ? event->event_id : "-",
             rule ? rule->id : "-",
             action ? claw_event_router_action_kind_to_string(action->kind) : "-");
    switch (action->kind) {
    case CLAW_EVENT_ROUTER_ACTION_CALL_CAP:
        return claw_event_router_execute_cap_action(rule, action, event, ctx, result);
    case CLAW_EVENT_ROUTER_ACTION_RUN_AGENT:
        return claw_event_router_execute_agent_action(rule, action, event, ctx, result);
    case CLAW_EVENT_ROUTER_ACTION_RUN_SCRIPT:
        return claw_event_router_execute_script_action(rule, action, event, ctx, result);
    case CLAW_EVENT_ROUTER_ACTION_SEND_MESSAGE:
        return claw_event_router_execute_send_message_action(rule, action, event, ctx, result);
    case CLAW_EVENT_ROUTER_ACTION_EMIT_EVENT:
        return claw_event_router_execute_emit_event_action(action, ctx, result);
    case CLAW_EVENT_ROUTER_ACTION_DROP:
        if (result) {
            result->action_count++;
            result->route = CLAW_CAP_EVENT_ROUTE_CONSUMED;
        }
        claw_event_router_update_last_output(ctx, "drop", "", "ok", "");
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t claw_event_router_run_default_agent(const claw_event_t *event,
                                                     claw_event_router_result_t *result)
{
    claw_event_router_action_t action = {
        .kind = CLAW_EVENT_ROUTER_ACTION_RUN_AGENT,
        .input_json = strdup("{}"),
        .caller = CLAW_CAP_CALLER_SYSTEM,
        .capture_output = true,
    };
    cJSON *ctx = claw_event_router_build_event_context(event);
    esp_err_t err;

    if (!action.input_json || !ctx) {
        free(action.input_json);
        cJSON_Delete(ctx);
        return ESP_ERR_NO_MEM;
    }

    err = claw_event_router_execute_agent_action(&(claw_event_router_rule_t) {
        .id = "__default_agent__",
    },
    &action,
    event,
    ctx,
    result);
    cJSON_Delete(ctx);
    free(action.input_json);
    return err;
}

static esp_err_t claw_event_router_process_event(const claw_event_t *event,
                                                 claw_event_router_result_t *out_result)
{
    claw_event_router_result_t local = {0};
    cJSON *ctx = NULL;

    if (!event) {
        return ESP_ERR_INVALID_ARG;
    }

    local.route = CLAW_CAP_EVENT_ROUTE_PASS;
    local.handled_at_ms = claw_event_router_now_ms();
    strlcpy(local.ack, "processing", sizeof(local.ack));

    claw_event_router_lock();
    s_runtime->last_result = local;
    claw_event_router_unlock();

    if (strcmp(event->source_cap, "claw_event_router") == 0) {
        if (out_result) {
            *out_result = local;
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG,
             "processing event=%s type=%s source=%s channel=%s chat=%s",
             event->event_id,
             event->event_type,
             event->source_cap,
             event->source_channel,
             event->chat_id);

    claw_event_router_lock();
    ctx = claw_event_router_build_event_context(event);
    if (!ctx) {
        claw_event_router_unlock();
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < s_runtime->rule_count; i++) {
        claw_event_router_rule_t *rule = &s_runtime->rules[i];
        claw_event_router_text_match_context_t match_ctx = {0};
        cJSON *rule_obj = NULL;
        cJSON *vars_obj = NULL;
        esp_err_t rule_err = ESP_OK;

        if (!claw_event_router_rule_matches(rule, event, &match_ctx)) {
            continue;
        }

        ESP_LOGI(TAG,
                 "event=%s matched rule=%s actions=%u",
                 event->event_id,
                 rule->id,
                 (unsigned int)rule->action_count);

        local.matched = true;
        local.matched_rules++;
        if (rule->consume_on_match) {
            local.route = CLAW_CAP_EVENT_ROUTE_CONSUMED;
        }
        if (!local.first_rule_id[0]) {
            strlcpy(local.first_rule_id, rule->id, sizeof(local.first_rule_id));
        }

        rule_obj = cJSON_CreateObject();
        if (!rule_obj) {
            cJSON_Delete(ctx);
            claw_event_router_unlock();
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(rule_obj, "id", rule->id);
        cJSON_DeleteItemFromObjectCaseSensitive(ctx, "rule");
        cJSON_AddItemToObject(ctx, "rule", rule_obj);

        rule_err = claw_event_router_update_match_context(ctx, &match_ctx);
        if (rule_err != ESP_OK) {
            cJSON_Delete(ctx);
            claw_event_router_unlock();
            return rule_err;
        }

        cJSON_DeleteItemFromObjectCaseSensitive(ctx, "vars");
        vars_obj = rule->vars_json ? cJSON_Parse(rule->vars_json) : cJSON_CreateObject();
        if (!cJSON_IsObject(vars_obj)) {
            cJSON_Delete(vars_obj);
            vars_obj = cJSON_CreateObject();
        }
        if (!vars_obj) {
            cJSON_Delete(ctx);
            claw_event_router_unlock();
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(ctx, "vars", vars_obj);

        for (size_t j = 0; j < rule->action_count; j++) {
            rule_err = claw_event_router_execute_action(rule, &rule->actions[j], event, ctx, &local);
            ESP_LOG_LEVEL(rule_err == ESP_OK ? ESP_LOG_DEBUG : ESP_LOG_WARN,
                          TAG,
                          "event=%s rule=%s action=%s done err=%s",
                          event->event_id,
                          rule->id,
                          claw_event_router_action_kind_to_string(rule->actions[j].kind),
                          esp_err_to_name(rule_err));
            if (rule_err != ESP_OK && !rule->actions[j].fail_open) {
                break;
            }
        }

        if (rule->ack[0]) {
            char *rendered_ack = claw_event_router_render_string(rule->ack, ctx);
            if (rendered_ack) {
                strlcpy(local.ack, rendered_ack, sizeof(local.ack));
                free(rendered_ack);
            }
        }
        if (rule->consume_on_match) {
            break;
        }
    }

    if (!local.matched &&
            s_runtime->config.default_route_messages_to_agent &&
            strcmp(event->event_type, "message") == 0 &&
            event->text && event->text[0]) {
        esp_err_t err;
        claw_event_router_result_t fallback = local;

        claw_event_router_unlock();
        cJSON_Delete(ctx);
        err = claw_event_router_run_default_agent(event, &fallback);
        claw_event_router_lock();
        s_runtime->last_result = fallback;
        claw_event_router_unlock();
        if (out_result) {
            *out_result = fallback;
        }
        return err;
    }

    if (local.matched && !local.ack[0]) {
        snprintf(local.ack, sizeof(local.ack), "matched:%s",
                 local.first_rule_id[0] ? local.first_rule_id : "(unknown)");
    }
    s_runtime->last_result = local;
    cJSON_Delete(ctx);
    claw_event_router_unlock();

    if (out_result) {
        *out_result = local;
    }
    return ESP_OK;
}

static void claw_event_router_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "event router task started");

    while (!s_runtime->stop_requested) {
        claw_event_t event = {0};
        claw_event_router_result_t result = {0};

        if (xQueueReceive(s_runtime->event_queue, &event, pdMS_TO_TICKS(250)) != pdTRUE) {
            continue;
        }
        if (pending_take_for_event_id(event.event_id)) {
            ESP_LOGI(TAG, "Skipping cancelled event %s (type=%s, source=%s)",
                     event.event_id,
                     event.event_type[0] ? event.event_type : "(none)",
                     event.source_cap[0] ? event.source_cap : "(none)");
            claw_event_free(&event);
            continue;
        }
        if (claw_event_router_process_event(&event, &result) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to process event %s", event.event_id);
        }
        claw_event_free(&event);
    }

    s_runtime->task_handle = NULL;
    s_runtime->started = false;
    claw_task_delete(NULL);
}

esp_err_t claw_event_router_init(const claw_event_router_config_t *config)
{
    if (s_runtime && s_runtime->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!config || !config->rules_path || !config->rules_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    s_runtime = calloc(1, sizeof(*s_runtime));
    if (!s_runtime) {
        return ESP_ERR_NO_MEM;
    }
    claw_event_router_init_defaults(s_runtime);
    s_runtime->mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_runtime->mutex) {
        claw_event_router_free_runtime();
        return ESP_ERR_NO_MEM;
    }

    memset(&s_runtime->config, 0, sizeof(s_runtime->config));
    s_runtime->config.task_core = tskNO_AFFINITY;
    if (config) {
        s_runtime->config = *config;
    }
    strlcpy(s_runtime->rules_path, config->rules_path, sizeof(s_runtime->rules_path));
    if (config && config->max_rules > 0) {
        s_runtime->max_rules = config->max_rules;
    }
    if (config && config->max_actions_per_rule > 0) {
        s_runtime->max_actions_per_rule = config->max_actions_per_rule;
    }
    if (config && config->cap_output_size > 0) {
        s_runtime->cap_output_size = config->cap_output_size;
    }

    uint32_t queue_len = config && config->event_queue_len ? config->event_queue_len
                                                            : CLAW_EVENT_ROUTER_DEFAULT_QUEUE_LEN;
    if (queue_len > CLAW_EVENT_ROUTER_PENDING_TABLE_SIZE) {
        ESP_LOGE(TAG, "event_queue_len=%u exceeds pending table size %u",
                 (unsigned)queue_len,
                 (unsigned)CLAW_EVENT_ROUTER_PENDING_TABLE_SIZE);
        claw_event_router_free_runtime();
        return ESP_ERR_INVALID_ARG;
    }
    s_runtime->event_queue = xQueueCreate(queue_len, sizeof(claw_event_t));
    if (!s_runtime->event_queue) {
        claw_event_router_free_runtime();
        return ESP_ERR_NO_MEM;
    }

    s_runtime->initialized = true;
    ESP_LOGI(TAG, "Rules path: %s", s_runtime->rules_path);
    {
        esp_err_t err = claw_event_router_reload();
        if (err != ESP_OK) {
            claw_event_router_free_runtime();
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t claw_event_router_start(void)
{
    BaseType_t task_ok;
    uint32_t stack_size;
    UBaseType_t priority;
    BaseType_t core;

    if (!s_runtime || !s_runtime->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime->started) {
        return ESP_OK;
    }

    stack_size = s_runtime->config.task_stack_size ?
                 s_runtime->config.task_stack_size : CLAW_EVENT_ROUTER_DEFAULT_STACK;
    priority = s_runtime->config.task_priority ?
               s_runtime->config.task_priority : CLAW_EVENT_ROUTER_DEFAULT_PRIO;
    core = s_runtime->config.task_core;
    s_runtime->config.agent_submit_timeout_ms = s_runtime->config.agent_submit_timeout_ms ?
                                                s_runtime->config.agent_submit_timeout_ms :
                                                CLAW_EVENT_ROUTER_DEFAULT_SUBMIT;
    s_runtime->stop_requested = false;

    task_ok = claw_task_create(&(claw_task_config_t){
                                   .name = "event_router",
                                   .stack_size = stack_size,
                                   .priority = priority,
                                   .core_id = core,
                                   .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                               },
                               claw_event_router_task,
                               NULL,
                               &s_runtime->task_handle);
    if (task_ok != pdPASS) {
        s_runtime->task_handle = NULL;
        return ESP_FAIL;
    }

    s_runtime->started = true;
    return ESP_OK;
}

esp_err_t claw_event_router_stop(void)
{
    TickType_t deadline;

    if (!s_runtime || !s_runtime->started || !s_runtime->task_handle) {
        return ESP_OK;
    }

    s_runtime->stop_requested = true;
    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    while (s_runtime->task_handle && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return s_runtime->task_handle ? ESP_ERR_TIMEOUT : ESP_OK;
}

esp_err_t claw_event_router_reload(void)
{
    claw_event_router_rule_t *new_rules = NULL;
    size_t new_rule_count = 0;
    esp_err_t err;

    if (!s_runtime || !s_runtime->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    err = claw_event_router_load_rules_from_file(s_runtime->rules_path,
                                                 &new_rules,
                                                 &new_rule_count,
                                                 NULL);
    if (err != ESP_OK) {
        return err;
    }

    claw_event_router_lock();
    claw_event_router_free_rules(s_runtime->rules, s_runtime->rule_count);
    s_runtime->rules = new_rules;
    s_runtime->rule_count = new_rule_count;
    claw_event_router_unlock();

    ESP_LOGI(TAG, "Loaded %u router rules", (unsigned int)new_rule_count);
    return ESP_OK;
}

esp_err_t claw_event_router_cancel_event(const char *event_id)
{
    bool armed = false;

    if (!s_runtime || !s_runtime->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!event_id || !event_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_event_router_lock();
    int slot = pending_find_slot_locked(event_id);
    if (slot >= 0 && !s_runtime->pending[slot].cancelled) {
        s_runtime->pending[slot].cancelled = true;
        armed = true;
        ESP_LOGI(TAG, "Cancel armed for queued event %s (type=%s, source=%s)",
                 s_runtime->pending[slot].event_id,
                 s_runtime->pending[slot].event_type,
                 s_runtime->pending[slot].source_cap);
    }
    claw_event_router_unlock();
    return armed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t claw_event_router_purge_queue(const char *event_type_filter,
                                        const char *source_cap_filter,
                                        size_t *out_cancelled)
{
    size_t armed = 0;

    if (!s_runtime || !s_runtime->initialized) {
        if (out_cancelled) {
            *out_cancelled = 0;
        }
        return ESP_ERR_INVALID_STATE;
    }

    claw_event_router_lock();
    for (int i = 0; i < (int)CLAW_EVENT_ROUTER_PENDING_TABLE_SIZE; i++) {
        if (!s_runtime->pending[i].used || s_runtime->pending[i].cancelled) {
            continue;
        }
        if (!pending_match_filter(&s_runtime->pending[i],
                                  event_type_filter, source_cap_filter)) {
            continue;
        }
        s_runtime->pending[i].cancelled = true;
        armed++;
    }
    claw_event_router_unlock();

    ESP_LOGI(TAG, "Purge queued events: type=%s source=%s armed=%u",
             (event_type_filter && event_type_filter[0]) ? event_type_filter : "(any)",
             (source_cap_filter && source_cap_filter[0]) ? source_cap_filter : "(any)",
             (unsigned)armed);
    if (out_cancelled) {
        *out_cancelled = armed;
    }
    return ESP_OK;
}

esp_err_t claw_event_router_publish(const claw_event_t *event)
{
    claw_event_t cloned = {0};
    esp_err_t err;

    if (!s_runtime || !s_runtime->initialized || !event || !event->source_cap[0] || !event->event_type[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_event_clone(event, &cloned);
    if (err != ESP_OK) {
        return err;
    }
    pending_track(&cloned);
    if (xQueueSend(s_runtime->event_queue, &cloned, pdMS_TO_TICKS(1000)) != pdTRUE) {
        (void)pending_take_for_event_id(cloned.event_id);
        claw_event_free(&cloned);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t claw_event_router_publish_message(const char *source_cap,
                                            const char *channel,
                                            const char *chat_id,
                                            const char *text,
                                            const char *sender_id,
                                            const char *message_id)
{
    claw_event_t event = {0};

    if (!source_cap || !channel || !chat_id || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(event.source_cap, source_cap, sizeof(event.source_cap));
    strlcpy(event.event_type, "message", sizeof(event.event_type));
    strlcpy(event.source_channel, channel, sizeof(event.source_channel));
    strlcpy(event.chat_id, chat_id, sizeof(event.chat_id));
    strlcpy(event.content_type, "text", sizeof(event.content_type));
    if (sender_id) {
        strlcpy(event.sender_id, sender_id, sizeof(event.sender_id));
    }
    if (message_id) {
        strlcpy(event.message_id, message_id, sizeof(event.message_id));
        strlcpy(event.correlation_id, message_id, sizeof(event.correlation_id));
    }
    event.timestamp_ms = claw_event_router_now_ms();
    event.session_policy = CLAW_SESSION_POLICY_CHAT;
    snprintf(event.event_id, sizeof(event.event_id), "msg-%" PRId64, event.timestamp_ms);
    event.text = (char *)text;
    return claw_event_router_publish(&event);
}

esp_err_t claw_event_router_publish_trigger(const char *source_cap,
                                            const char *event_type,
                                            const char *event_key,
                                            const char *payload_json)
{
    claw_event_t event = {0};

    if (!source_cap || !event_type || !event_key) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(event.source_cap, source_cap, sizeof(event.source_cap));
    strlcpy(event.event_type, event_type, sizeof(event.event_type));
    strlcpy(event.message_id, event_key, sizeof(event.message_id));
    strlcpy(event.correlation_id, event_key, sizeof(event.correlation_id));
    strlcpy(event.content_type, "trigger", sizeof(event.content_type));
    event.timestamp_ms = claw_event_router_now_ms();
    event.session_policy = CLAW_SESSION_POLICY_TRIGGER;
    snprintf(event.event_id, sizeof(event.event_id), "evt-%" PRId64, event.timestamp_ms);
    event.payload_json = (char *)(payload_json ? payload_json : "{}");
    return claw_event_router_publish(&event);
}

esp_err_t claw_event_router_register_outbound_binding(const char *channel,
                                                      const char *cap_name)
{
    if (!s_runtime || !s_runtime->initialized || !channel || !channel[0] ||
            !cap_name || !cap_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_event_router_lock();
    for (size_t i = 0; i < s_runtime->binding_count; i++) {
        if (strcmp(s_runtime->bindings[i].channel, channel) == 0) {
            strlcpy(s_runtime->bindings[i].cap_name,
                    cap_name,
                    sizeof(s_runtime->bindings[i].cap_name));
            claw_event_router_unlock();
            return ESP_OK;
        }
    }
    if (s_runtime->binding_count >= CLAW_EVENT_ROUTER_BINDING_SIZE) {
        claw_event_router_unlock();
        return ESP_ERR_NO_MEM;
    }
    strlcpy(s_runtime->bindings[s_runtime->binding_count].channel,
            channel,
            sizeof(s_runtime->bindings[s_runtime->binding_count].channel));
    strlcpy(s_runtime->bindings[s_runtime->binding_count].cap_name,
            cap_name,
            sizeof(s_runtime->bindings[s_runtime->binding_count].cap_name));
    s_runtime->binding_count++;
    claw_event_router_unlock();
    return ESP_OK;
}

esp_err_t claw_event_router_handle_event(const claw_event_t *event,
                                         claw_event_router_result_t *out_result)
{
    if (!s_runtime || !s_runtime->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return claw_event_router_process_event(event, out_result);
}

esp_err_t claw_event_router_list_rules(claw_event_router_rule_t **out_rules,
                                       size_t *out_rule_count)
{
    if (!s_runtime || !s_runtime->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return claw_event_router_load_rules_from_file(s_runtime->rules_path,
                                                  out_rules,
                                                  out_rule_count,
                                                  NULL);
}

esp_err_t claw_event_router_get_rule(const char *id, claw_event_router_rule_t *out_rule)
{
    claw_event_router_rule_t *rules = NULL;
    size_t rule_count = 0;
    esp_err_t err;

    if (!id || !id[0] || !out_rule) {
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_event_router_list_rules(&rules, &rule_count);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < rule_count; i++) {
        if (strcmp(rules[i].id, id) == 0) {
            *out_rule = rules[i];
            memset(&rules[i], 0, sizeof(rules[i]));
            claw_event_router_free_rule_list(rules, rule_count);
            return ESP_OK;
        }
    }

    claw_event_router_free_rule_list(rules, rule_count);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t claw_event_router_add_rule(const claw_event_router_rule_t *rule)
{
    claw_event_router_rule_t *loaded_rules = NULL;
    claw_event_router_rule_t *new_rules = NULL;
    size_t old_rule_count = 0;
    size_t new_rule_count = 0;
    cJSON *root = NULL;
    cJSON *rule_json = NULL;
    esp_err_t err;

    if (!s_runtime || !s_runtime->initialized || !rule) {
        return (s_runtime && s_runtime->initialized) ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }

    err = claw_event_router_load_rules_from_file(s_runtime->rules_path,
                                                 &loaded_rules,
                                                 &old_rule_count,
                                                 &root);
    if (err != ESP_OK) {
        return err;
    }
    if (old_rule_count >= s_runtime->max_rules) {
        cJSON_Delete(root);
        claw_event_router_free_rule_list(loaded_rules, old_rule_count);
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < old_rule_count; i++) {
        if (strcmp(loaded_rules[i].id, rule->id) == 0) {
            cJSON_Delete(root);
            claw_event_router_free_rule_list(loaded_rules, old_rule_count);
            return ESP_ERR_INVALID_STATE;
        }
    }

    rule_json = claw_event_router_rule_to_json(rule);
    if (!rule_json) {
        cJSON_Delete(root);
        claw_event_router_free_rule_list(loaded_rules, old_rule_count);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_InsertItemInArray(root, 0, rule_json);
    claw_event_router_free_rule_list(loaded_rules, old_rule_count);

    err = claw_event_router_load_rules_from_root(root, &new_rules, &new_rule_count);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    return claw_event_router_commit_rules(root, new_rules, new_rule_count);
}

esp_err_t claw_event_router_update_rule(const claw_event_router_rule_t *rule)
{
    claw_event_router_rule_t *loaded_rules = NULL;
    claw_event_router_rule_t *new_rules = NULL;
    size_t old_rule_count = 0;
    size_t new_rule_count = 0;
    cJSON *root = NULL;
    cJSON *rule_json = NULL;
    cJSON *old_item = NULL;
    esp_err_t err;
    bool found = false;

    if (!s_runtime || !s_runtime->initialized || !rule) {
        return (s_runtime && s_runtime->initialized) ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }

    err = claw_event_router_load_rules_from_file(s_runtime->rules_path,
                                                 &loaded_rules,
                                                 &old_rule_count,
                                                 &root);
    if (err != ESP_OK) {
        return err;
    }

    cJSON_ArrayForEach(old_item, root) {
        const char *rule_id = cJSON_GetStringValue(cJSON_GetObjectItem(old_item, "id"));
        if (rule_id && strcmp(rule_id, rule->id) == 0) {
            rule_json = claw_event_router_rule_to_json(rule);
            if (!rule_json) {
                cJSON_Delete(root);
                claw_event_router_free_rule_list(loaded_rules, old_rule_count);
                return ESP_ERR_INVALID_ARG;
            }
            cJSON_ReplaceItemViaPointer(root, old_item, rule_json);
            found = true;
            break;
        }
    }
    claw_event_router_free_rule_list(loaded_rules, old_rule_count);
    if (!found) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    err = claw_event_router_load_rules_from_root(root, &new_rules, &new_rule_count);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    return claw_event_router_commit_rules(root, new_rules, new_rule_count);
}

esp_err_t claw_event_router_delete_rule(const char *id)
{
    claw_event_router_rule_t *loaded_rules = NULL;
    claw_event_router_rule_t *new_rules = NULL;
    size_t old_rule_count = 0;
    size_t new_rule_count = 0;
    cJSON *root = NULL;
    esp_err_t err;
    bool found = false;

    if (!s_runtime || !s_runtime->initialized || !id || !id[0]) {
        return (s_runtime && s_runtime->initialized) ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }

    err = claw_event_router_load_rules_from_file(s_runtime->rules_path,
                                                 &loaded_rules,
                                                 &old_rule_count,
                                                 &root);
    if (err != ESP_OK) {
        return err;
    }
    claw_event_router_free_rule_list(loaded_rules, old_rule_count);

    for (int i = 0; i < cJSON_GetArraySize(root); i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        const char *rule_id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));

        if (rule_id && strcmp(rule_id, id) == 0) {
            cJSON_DeleteItemFromArray(root, i);
            found = true;
            break;
        }
    }
    if (!found) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    err = claw_event_router_load_rules_from_root(root, &new_rules, &new_rule_count);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    return claw_event_router_commit_rules(root, new_rules, new_rule_count);
}

esp_err_t claw_event_router_add_rule_json(const char *rule_json)
{
    claw_event_router_rule_t *rule = NULL;
    esp_err_t err;

    rule = calloc(1, sizeof(*rule));
    if (!rule) {
        return ESP_ERR_NO_MEM;
    }

    err = claw_event_router_parse_rule_json(rule_json, rule);
    if (err != ESP_OK) {
        free(rule);
        return err;
    }
    err = claw_event_router_add_rule(rule);
    claw_event_router_free_rule(rule);
    free(rule);
    return err;
}

esp_err_t claw_event_router_update_rule_json(const char *rule_json)
{
    claw_event_router_rule_t *rule = NULL;
    esp_err_t err;

    rule = calloc(1, sizeof(*rule));
    if (!rule) {
        return ESP_ERR_NO_MEM;
    }

    err = claw_event_router_parse_rule_json(rule_json, rule);
    if (err != ESP_OK) {
        free(rule);
        return err;
    }
    err = claw_event_router_update_rule(rule);
    claw_event_router_free_rule(rule);
    free(rule);
    return err;
}

esp_err_t claw_event_router_list_rules_json(char *output, size_t output_size)
{
    char *buf = NULL;
    cJSON *root = NULL;
    char *json = NULL;
    esp_err_t err;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_runtime || !s_runtime->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    err = claw_event_router_read_file(s_runtime->rules_path, &buf);
    if (err == ESP_ERR_NOT_FOUND) {
        strlcpy(output, "[]", output_size);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, json, output_size);
    free(json);
    return ESP_OK;
}

esp_err_t claw_event_router_get_rule_json(const char *id, char *output, size_t output_size)
{
    claw_event_router_rule_t rule = {0};
    cJSON *item = NULL;
    char *json = NULL;
    esp_err_t err;

    if (!id || !id[0] || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_event_router_get_rule(id, &rule);
    if (err != ESP_OK) {
        return err;
    }

    item = claw_event_router_rule_to_json(&rule);
    claw_event_router_free_rule(&rule);
    if (!item) {
        return ESP_ERR_INVALID_ARG;
    }
    json = cJSON_PrintUnformatted(item);
    cJSON_Delete(item);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    strlcpy(output, json, output_size);
    free(json);
    return ESP_OK;
}

esp_err_t claw_event_router_get_last_result(claw_event_router_result_t *out_result)
{
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_runtime || !s_runtime->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    claw_event_router_lock();
    *out_result = s_runtime->last_result;
    claw_event_router_unlock();
    return ESP_OK;
}
