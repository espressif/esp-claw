/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap_scheduler_internal.h"
#include "claw_cabi_esp.h"
#include "claw_task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

static const char *TAG = "cap_scheduler";

EXT_RAM_BSS_ATTR cap_scheduler_runtime_t s_cap_scheduler = {0};

static esp_err_t cap_scheduler_execute_list_impl(const char *input_json,
                                                 char *output,
                                                 size_t output_size);
static esp_err_t cap_scheduler_execute_get_impl(const char *input_json,
                                                char *output,
                                                size_t output_size);
static esp_err_t cap_scheduler_execute_add_impl(const char *input_json,
                                                char *output,
                                                size_t output_size);
static esp_err_t cap_scheduler_execute_update_impl(const char *input_json,
                                                   char *output,
                                                   size_t output_size);
static esp_err_t cap_scheduler_execute_enable_impl(const char *input_json,
                                                   char *output,
                                                   size_t output_size);
static esp_err_t cap_scheduler_execute_disable_impl(const char *input_json,
                                                    char *output,
                                                    size_t output_size);
static esp_err_t cap_scheduler_execute_remove_impl(const char *input_json,
                                                   char *output,
                                                   size_t output_size);
static esp_err_t cap_scheduler_execute_pause_impl(const char *input_json,
                                                  char *output,
                                                  size_t output_size);
static esp_err_t cap_scheduler_execute_resume_impl(const char *input_json,
                                                   char *output,
                                                   size_t output_size);
static esp_err_t cap_scheduler_execute_trigger_impl(const char *input_json,
                                                    char *output,
                                                    size_t output_size);
static esp_err_t cap_scheduler_execute_reload_impl(const char *input_json,
                                                   char *output,
                                                   size_t output_size);

CLAW_CABI_ESP_TOOL_CALLBACK(cap_scheduler_execute_list, cap_scheduler_execute_list_impl)
CLAW_CABI_ESP_TOOL_CALLBACK(cap_scheduler_execute_get, cap_scheduler_execute_get_impl)
CLAW_CABI_ESP_TOOL_CALLBACK(cap_scheduler_execute_add, cap_scheduler_execute_add_impl)
CLAW_CABI_ESP_TOOL_CALLBACK(cap_scheduler_execute_update, cap_scheduler_execute_update_impl)
CLAW_CABI_ESP_TOOL_CALLBACK(cap_scheduler_execute_enable, cap_scheduler_execute_enable_impl)
CLAW_CABI_ESP_TOOL_CALLBACK(cap_scheduler_execute_disable, cap_scheduler_execute_disable_impl)
CLAW_CABI_ESP_TOOL_CALLBACK(cap_scheduler_execute_remove, cap_scheduler_execute_remove_impl)
CLAW_CABI_ESP_TOOL_CALLBACK(cap_scheduler_execute_pause, cap_scheduler_execute_pause_impl)
CLAW_CABI_ESP_TOOL_CALLBACK(cap_scheduler_execute_resume, cap_scheduler_execute_resume_impl)
CLAW_CABI_ESP_TOOL_CALLBACK(cap_scheduler_execute_trigger, cap_scheduler_execute_trigger_impl)
CLAW_CABI_ESP_TOOL_CALLBACK(cap_scheduler_execute_reload, cap_scheduler_execute_reload_impl)

static const claw_capability_t s_scheduler_descriptors[] = {
    CLAW_CABI_ESP_TOOL_DESCRIPTOR(
        "scheduler_list",
        "List all scheduler entries and runtime state.",
        "{\"type\":\"object\",\"properties\":{}}",
        cap_scheduler_execute_list),
    CLAW_CABI_ESP_TOOL_DESCRIPTOR(
        "scheduler_get",
        "Get one scheduler entry by id.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
        cap_scheduler_execute_get),
    CLAW_CABI_ESP_TOOL_DESCRIPTOR(
        "scheduler_add",
        "Add one scheduler entry from schedule_json string and return runtime state.",
        "{\"type\":\"object\",\"properties\":{\"schedule_json\":{\"type\":\"string\"}},\"required\":[\"schedule_json\"]}",
        cap_scheduler_execute_add),
    CLAW_CABI_ESP_TOOL_DESCRIPTOR(
        "scheduler_update",
        "Update one scheduler entry from schedule_json string and return runtime state.",
        "{\"type\":\"object\",\"properties\":{\"schedule_json\":{\"type\":\"string\"}},\"required\":[\"schedule_json\"]}",
        cap_scheduler_execute_update),
    CLAW_CABI_ESP_TOOL_DESCRIPTOR(
        "scheduler_enable",
        "Enable one scheduler entry by id.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
        cap_scheduler_execute_enable),
    CLAW_CABI_ESP_TOOL_DESCRIPTOR(
        "scheduler_disable",
        "Disable one scheduler entry by id.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
        cap_scheduler_execute_disable),
    CLAW_CABI_ESP_TOOL_DESCRIPTOR(
        "scheduler_remove",
        "Remove one scheduler entry by id.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
        cap_scheduler_execute_remove),
    CLAW_CABI_ESP_TOOL_DESCRIPTOR(
        "scheduler_pause",
        "Pause one scheduler entry by id.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
        cap_scheduler_execute_pause),
    CLAW_CABI_ESP_TOOL_DESCRIPTOR(
        "scheduler_resume",
        "Resume one scheduler entry by id.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
        cap_scheduler_execute_resume),
    CLAW_CABI_ESP_TOOL_DESCRIPTOR(
        "scheduler_trigger_now",
        "Trigger one scheduler entry immediately.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
        cap_scheduler_execute_trigger),
    CLAW_CABI_ESP_TOOL_DESCRIPTOR(
        "scheduler_reload",
        "Reload scheduler definitions from disk.",
        "{\"type\":\"object\",\"properties\":{}}",
        cap_scheduler_execute_reload),
};

static const claw_capability_group_t s_scheduler_group = {
    .id = "cap_scheduler",
    .members = s_scheduler_descriptors,
    .member_count = sizeof(s_scheduler_descriptors) / sizeof(s_scheduler_descriptors[0]),
};

static const char *cap_scheduler_kind_to_string_local(cap_scheduler_item_kind_t kind)
{
    switch (kind) {
    case CAP_SCHEDULER_ITEM_ONCE:
        return "once";
    case CAP_SCHEDULER_ITEM_INTERVAL:
        return "interval";
    case CAP_SCHEDULER_ITEM_CRON:
        return "cron";
    default:
        return "unknown";
    }
}

static bool cap_scheduler_parse_session_policy_local(const char *value,
                                                     claw_session_policy_t *out_policy)
{
    if (!out_policy) {
        return false;
    }
    if (!value || !value[0] || strcmp(value, "trigger") == 0) {
        *out_policy = CLAW_SESSION_POLICY_TRIGGER;
        return true;
    }
    if (strcmp(value, "chat") == 0) {
        *out_policy = CLAW_SESSION_POLICY_CHAT;
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

static bool cap_scheduler_item_requires_valid_time(const cap_scheduler_item_t *item)
{
    if (!item) {
        return true;
    }

    if (item->kind == CAP_SCHEDULER_ITEM_INTERVAL &&
            item->start_at_ms <= 0 &&
            item->end_at_ms <= 0) {
        return false;
    }

    return true;
}

static void cap_scheduler_lock(void)
{
    xSemaphoreTakeRecursive(s_cap_scheduler.mutex, portMAX_DELAY);
}

static void cap_scheduler_unlock(void)
{
    xSemaphoreGiveRecursive(s_cap_scheduler.mutex);
}

static size_t cap_scheduler_active_count_locked(void)
{
    size_t count = 0;

    for (size_t i = 0; i < s_cap_scheduler.max_items; i++) {
        if (s_cap_scheduler.entries[i].occupied) {
            count++;
        }
    }
    return count;
}

static ssize_t cap_scheduler_find_entry_index_locked(const char *id)
{
    if (!id || !id[0]) {
        return -1;
    }

    for (size_t i = 0; i < s_cap_scheduler.max_items; i++) {
        if (s_cap_scheduler.entries[i].occupied &&
                strcmp(s_cap_scheduler.entries[i].item.id, id) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static ssize_t cap_scheduler_find_free_index_locked(void)
{
    for (size_t i = 0; i < s_cap_scheduler.max_items; i++) {
        if (!s_cap_scheduler.entries[i].occupied) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static esp_err_t cap_scheduler_persist_definitions_locked(void)
{
    return cap_scheduler_save_items(s_cap_scheduler.schedules_path,
                                    s_cap_scheduler.entries,
                                    s_cap_scheduler.max_items);
}

static esp_err_t cap_scheduler_persist_runtime_state_locked(void)
{
    return cap_scheduler_save_state(s_cap_scheduler.state_path,
                                    s_cap_scheduler.entries,
                                    s_cap_scheduler.max_items);
}

static esp_err_t cap_scheduler_persist_all_locked(void)
{
    ESP_RETURN_ON_ERROR(cap_scheduler_persist_definitions_locked(),
                        TAG,
                        "Failed to save scheduler definitions");
    return cap_scheduler_persist_runtime_state_locked();
}

static esp_err_t cap_scheduler_load_runtime_state_locked(bool *runtime_state_loaded)
{
    esp_err_t primary_err;

    if (!runtime_state_loaded) {
        return ESP_ERR_INVALID_ARG;
    }

    *runtime_state_loaded = false;
    primary_err = cap_scheduler_load_state(s_cap_scheduler.state_path,
                                           s_cap_scheduler.entries,
                                           s_cap_scheduler.max_items);
    if (primary_err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded scheduler runtime state from primary %s",
                 s_cap_scheduler.state_path);
        *runtime_state_loaded = true;
        return ESP_OK;
    }

    if (primary_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load scheduler state from %s: %s",
                 s_cap_scheduler.state_path,
                 esp_err_to_name(primary_err));
    }

    ESP_LOGW(TAG,
             "Runtime state unavailable, rebuilding runtime state from %s only",
             s_cap_scheduler.schedules_path);
    return ESP_OK;
}

static esp_err_t cap_scheduler_refresh_entry_locked(cap_scheduler_entry_t *entry, int64_t now_ms)
{
    esp_err_t err;

    if (!entry || !entry->occupied) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!entry->item.enabled) {
        entry->status = CAP_SCHEDULER_STATUS_DISABLED;
        entry->next_fire_ms = -1;
        return ESP_OK;
    }
    if (entry->status == CAP_SCHEDULER_STATUS_PAUSED) {
        return ESP_OK;
    }
    if (!s_cap_scheduler.time_valid &&
            cap_scheduler_item_requires_valid_time(&entry->item)) {
        entry->status = CAP_SCHEDULER_STATUS_SCHEDULED;
        entry->next_fire_ms = -1;
        return ESP_OK;
    }

    err = cap_scheduler_compute_next_fire(&entry->item,
                                          now_ms,
                                          entry->run_count,
                                          &entry->next_fire_ms);
    if (err != ESP_OK) {
        entry->status = CAP_SCHEDULER_STATUS_ERROR;
        entry->last_error_code = err;
        return err;
    }
    if (entry->next_fire_ms < 0) {
        entry->status = CAP_SCHEDULER_STATUS_COMPLETED;
    } else {
        entry->status = CAP_SCHEDULER_STATUS_SCHEDULED;
    }
    return ESP_OK;
}

static esp_err_t cap_scheduler_build_payload_json(const cap_scheduler_entry_t *entry,
                                                  int64_t planned_time_ms,
                                                  int64_t fire_time_ms,
                                                  char *buf,
                                                  size_t buf_size)
{
    cJSON *root = NULL;
    cJSON *user_payload = NULL;
    char *rendered = NULL;

    if (!entry || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schedule_id", entry->item.id);
    cJSON_AddNumberToObject(root, "planned_time_ms", (double)planned_time_ms);
    cJSON_AddNumberToObject(root, "fire_time_ms", (double)fire_time_ms);
    cJSON_AddStringToObject(root, "kind",
                            entry->item.kind == CAP_SCHEDULER_ITEM_ONCE ? "once" :
                            entry->item.kind == CAP_SCHEDULER_ITEM_INTERVAL ? "interval" : "cron");
    cJSON_AddNumberToObject(root, "run_count", entry->run_count + 1);

    user_payload = cJSON_Parse(entry->item.payload_json[0] ? entry->item.payload_json : "{}");
    if (!user_payload) {
        user_payload = cJSON_CreateObject();
    }
    cJSON_AddItemToObject(root, "user_payload", user_payload);

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }

    strlcpy(buf, rendered, buf_size);
    free(rendered);
    return ESP_OK;
}

static esp_err_t cap_scheduler_publish_entry_locked(cap_scheduler_entry_t *entry,
                                                    bool immediate,
                                                    int64_t now_ms)
{
    claw_event_t event = {0};
    char payload_json[CAP_SCHEDULER_PAYLOAD_LEN] = {0};
    esp_err_t err;
    esp_err_t publish_err;
    int64_t planned_time_ms;

    if (!entry || !entry->occupied) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_cap_scheduler.config.publish_event) {
        return ESP_ERR_INVALID_STATE;
    }

    planned_time_ms = immediate ? now_ms : entry->next_fire_ms;
    if (planned_time_ms <= 0) {
        planned_time_ms = now_ms;
    }

    ESP_RETURN_ON_ERROR(cap_scheduler_build_payload_json(entry,
                                                         planned_time_ms,
                                                         now_ms,
                                                         payload_json,
                                                         sizeof(payload_json)),
                        TAG,
                        "Failed to build schedule payload");

    snprintf(event.event_id, sizeof(event.event_id), "sch-%08" PRIx32,
             (uint32_t)(now_ms & 0xffffffffU));
    strlcpy(event.source_cap, "cap_scheduler", sizeof(event.source_cap));
    strlcpy(event.event_type, entry->item.event_type, sizeof(event.event_type));
    strlcpy(event.source_channel, entry->item.source_channel, sizeof(event.source_channel));
    strlcpy(event.chat_id, entry->item.chat_id, sizeof(event.chat_id));
    strlcpy(event.message_id, entry->item.event_key, sizeof(event.message_id));
    snprintf(event.correlation_id, sizeof(event.correlation_id), "%s:%" PRId64, entry->item.id, planned_time_ms);
    strlcpy(event.content_type, entry->item.content_type, sizeof(event.content_type));
    event.timestamp_ms = now_ms;
    event.text = entry->item.text[0] ? entry->item.text : NULL;
    event.payload_json = payload_json;
    if (!cap_scheduler_parse_session_policy_local(entry->item.session_policy, &event.session_policy)) {
        event.session_policy = CLAW_SESSION_POLICY_TRIGGER;
    }

    entry->status = CAP_SCHEDULER_STATUS_RUNNING;
    publish_err = s_cap_scheduler.config.publish_event(&event);
    err = publish_err;
    entry->last_fire_ms = now_ms;
    entry->last_error_code = publish_err;
    if (publish_err == ESP_OK) {
        entry->last_success_ms = now_ms;
        entry->run_count++;
    }

    if (publish_err == ESP_OK && entry->item.kind == CAP_SCHEDULER_ITEM_ONCE) {
        entry->next_fire_ms = -1;
        entry->status = CAP_SCHEDULER_STATUS_COMPLETED;
    } else if (publish_err == ESP_OK && entry->status != CAP_SCHEDULER_STATUS_PAUSED) {
        if (!immediate) {
            err = cap_scheduler_refresh_entry_locked(entry, planned_time_ms);
        } else {
            err = cap_scheduler_refresh_entry_locked(entry, now_ms);
        }
    } else if (publish_err != ESP_OK) {
        entry->status = CAP_SCHEDULER_STATUS_ERROR;
    }

    if (publish_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "schedule triggered id=%s kind=%s immediate=%s planned_time_ms=%" PRId64
                 " fire_time_ms=%" PRId64 " run_count=%d next_fire_ms=%" PRId64
                 " event_type=%s event_key=%s",
                 entry->item.id,
                 cap_scheduler_kind_to_string_local(entry->item.kind),
                 immediate ? "true" : "false",
                 planned_time_ms,
                 now_ms,
                 entry->run_count,
                 entry->next_fire_ms,
                 entry->item.event_type,
                 entry->item.event_key);
    } else {
        ESP_LOGW(TAG,
                 "schedule trigger failed id=%s kind=%s immediate=%s planned_time_ms=%" PRId64
                 " fire_time_ms=%" PRId64 " err=%s event_type=%s event_key=%s",
                 entry->item.id,
                 cap_scheduler_kind_to_string_local(entry->item.kind),
                 immediate ? "true" : "false",
                 planned_time_ms,
                 now_ms,
                 esp_err_to_name(publish_err),
                 entry->item.event_type,
                 entry->item.event_key);
    }

    return err;
}

static esp_err_t cap_scheduler_fire_due_entries(void)
{
    esp_err_t overall = ESP_OK;
    int64_t now_ms = cap_scheduler_now_ms();
    bool runtime_state_changed = false;

    cap_scheduler_lock();
    for (size_t i = 0; i < s_cap_scheduler.max_items; i++) {
        cap_scheduler_entry_t *entry = &s_cap_scheduler.entries[i];

        if (!entry->occupied || !entry->item.enabled || entry->status == CAP_SCHEDULER_STATUS_PAUSED) {
            continue;
        }
        if (entry->next_fire_ms <= 0 || entry->next_fire_ms > now_ms) {
            continue;
        }
        if (now_ms > entry->next_fire_ms + (int64_t)s_cap_scheduler.config.tick_ms) {
            entry->missed_count++;
            runtime_state_changed = true;
            if (cap_scheduler_refresh_entry_locked(entry, now_ms) != ESP_OK) {
                overall = ESP_FAIL;
            }
            continue;
        }
        runtime_state_changed = true;
        if (cap_scheduler_publish_entry_locked(entry, false, now_ms) != ESP_OK) {
            overall = ESP_FAIL;
        }
    }
    if (s_cap_scheduler.config.persist_after_fire && runtime_state_changed) {
        cap_scheduler_persist_runtime_state_locked();
    }
    cap_scheduler_unlock();
    return overall;
}

static void cap_scheduler_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "scheduler task started");
    while (!s_cap_scheduler.stop_requested) {
        cap_scheduler_fire_due_entries();
        vTaskDelay(pdMS_TO_TICKS(s_cap_scheduler.config.tick_ms));
    }
    s_cap_scheduler.started = false;
    s_cap_scheduler.task_handle = NULL;
    claw_task_delete(NULL);
}

static esp_err_t cap_scheduler_load_from_disk_locked(void)
{
    cap_scheduler_item_t *items = NULL;
    size_t item_count = 0;
    int64_t now_ms = cap_scheduler_now_ms();
    esp_err_t err;
    bool runtime_state_loaded = false;

    items = calloc(s_cap_scheduler.max_items, sizeof(cap_scheduler_item_t));
    if (!items) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_scheduler_load_items(s_cap_scheduler.schedules_path, items, s_cap_scheduler.max_items, &item_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load schedules from %s: %s",
                 s_cap_scheduler.schedules_path,
                 esp_err_to_name(err));
        free(items);
        return err;
    }

    memset(s_cap_scheduler.entries, 0, s_cap_scheduler.max_items * sizeof(cap_scheduler_entry_t));
    for (size_t i = 0; i < item_count; i++) {
        s_cap_scheduler.entries[i].occupied = true;
        s_cap_scheduler.entries[i].item = items[i];
        s_cap_scheduler.entries[i].status = items[i].enabled ?
                                            CAP_SCHEDULER_STATUS_SCHEDULED : CAP_SCHEDULER_STATUS_DISABLED;
        s_cap_scheduler.entries[i].next_fire_ms = -1;
    }
    s_cap_scheduler.item_count = item_count;
    free(items);

    err = cap_scheduler_persist_definitions_locked();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save normalized scheduler definitions to %s: %s",
                 s_cap_scheduler.schedules_path,
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved normalized scheduler definitions to %s",
                 s_cap_scheduler.schedules_path);
    }

    err = cap_scheduler_load_runtime_state_locked(&runtime_state_loaded);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < s_cap_scheduler.max_items; i++) {
        if (!s_cap_scheduler.entries[i].occupied) {
            continue;
        }
        if (s_cap_scheduler.entries[i].status == CAP_SCHEDULER_STATUS_PAUSED) {
            continue;
        }
        err = cap_scheduler_refresh_entry_locked(&s_cap_scheduler.entries[i],
                                                    s_cap_scheduler.entries[i].last_fire_ms > 0 ?
                                                    s_cap_scheduler.entries[i].last_fire_ms : now_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to refresh schedule %s: %s",
                        s_cap_scheduler.entries[i].item.id,
                        esp_err_to_name(err));
        }
    }

    err = cap_scheduler_persist_runtime_state_locked();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save initial scheduler state to %s: %s",
                 s_cap_scheduler.state_path,
                 esp_err_to_name(err));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Loaded %u scheduler entries from %s",
             (unsigned)item_count,
             s_cap_scheduler.schedules_path);
    return ESP_OK;
}

static esp_err_t cap_scheduler_parse_id_input(const char *input_json, char *id, size_t id_size)
{
    cJSON *root = NULL;
    cJSON *id_item = NULL;

    if (!id || id_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    id_item = root ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
    if (!cJSON_IsString(id_item) || !id_item->valuestring || !id_item->valuestring[0]) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(id, id_item->valuestring, id_size);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_scheduler_parse_add_input(const char *input_json, cap_scheduler_item_t *item)
{
    cJSON *root = NULL;
    const char *schedule_json = NULL;
    esp_err_t err;

    if (!item) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    schedule_json = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "schedule_json"));
    if (!schedule_json || !schedule_json[0]) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_scheduler_parse_item_json_string(schedule_json, item);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_scheduler_write_snapshot_json(const cap_scheduler_snapshot_t *snapshot,
                                                   char *output,
                                                   size_t output_size)
{
    cap_scheduler_entry_t *entry = NULL;
    cJSON *root = NULL;
    char *rendered = NULL;
    esp_err_t err;

    if (!snapshot || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return ESP_ERR_NO_MEM;
    }

    entry->occupied = true;
    entry->item = snapshot->item;
    entry->status = snapshot->status;
    entry->next_fire_ms = snapshot->next_fire_ms;
    entry->last_fire_ms = snapshot->last_fire_ms;
    entry->last_success_ms = snapshot->last_success_ms;
    entry->run_count = snapshot->run_count;
    entry->missed_count = snapshot->missed_count;
    entry->last_error_code = snapshot->last_error_code;
    err = cap_scheduler_entry_to_json(entry, true, &root);
    free(entry);
    if (err != ESP_OK) {
        return err;
    }
    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, rendered, output_size);
    free(rendered);
    return ESP_OK;
}

esp_err_t cap_scheduler_register_group(claw_capability_registry_t *registry)
{
    if (!registry) {
        return ESP_ERR_INVALID_ARG;
    }
    return claw_cabi_register_group_esp(registry, &s_scheduler_group);
}

esp_err_t cap_scheduler_init(const cap_scheduler_config_t *config)
{
    const char *schedules_path = NULL;

    if (s_cap_scheduler.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!config || !config->schedules_path || !config->schedules_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_cap_scheduler.mutex) {
        s_cap_scheduler.mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (!s_cap_scheduler.mutex) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_cap_scheduler.config, 0, sizeof(s_cap_scheduler.config));
    if (config) {
        s_cap_scheduler.config = *config;
    }

    s_cap_scheduler.config.tick_ms = s_cap_scheduler.config.tick_ms ?
                                     s_cap_scheduler.config.tick_ms : CAP_SCHEDULER_DEFAULT_TICK_MS;
    s_cap_scheduler.config.max_items = s_cap_scheduler.config.max_items ?
                                       s_cap_scheduler.config.max_items : CAP_SCHEDULER_DEFAULT_MAX_ITEMS;
    s_cap_scheduler.config.task_stack_size = s_cap_scheduler.config.task_stack_size ?
                                             s_cap_scheduler.config.task_stack_size : CAP_SCHEDULER_DEFAULT_STACK;
    s_cap_scheduler.config.task_priority = s_cap_scheduler.config.task_priority ?
                                           s_cap_scheduler.config.task_priority : CAP_SCHEDULER_DEFAULT_PRIORITY;
    s_cap_scheduler.config.task_core = config ? config->task_core : tskNO_AFFINITY;

    schedules_path = config->schedules_path;
    strlcpy(s_cap_scheduler.schedules_path, schedules_path, sizeof(s_cap_scheduler.schedules_path));
    ESP_RETURN_ON_ERROR(cap_scheduler_build_state_path(schedules_path, s_cap_scheduler.state_path, sizeof(s_cap_scheduler.state_path)),
                        TAG,
                        "Failed to derive scheduler state key");
    s_cap_scheduler.max_items = s_cap_scheduler.config.max_items;
    s_cap_scheduler.entries = calloc(s_cap_scheduler.max_items, sizeof(cap_scheduler_entry_t));
    if (!s_cap_scheduler.entries) {
        return ESP_ERR_NO_MEM;
    }
    s_cap_scheduler.time_valid = false;
    s_cap_scheduler.initialized = true;

    cap_scheduler_lock();
    {
        esp_err_t err = cap_scheduler_load_from_disk_locked();

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Scheduler init load failed: %s", esp_err_to_name(err));
            cap_scheduler_unlock();
            return err;
        }
    }
    cap_scheduler_unlock();
    return ESP_OK;
}

esp_err_t cap_scheduler_start(void)
{
    BaseType_t ok;
    claw_task_config_t task_config = {
        .name = "cap_scheduler",
        .stack_size = s_cap_scheduler.config.task_stack_size,
        .priority = s_cap_scheduler.config.task_priority,
        .core_id = s_cap_scheduler.config.task_core,
        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
    };

    if (!s_cap_scheduler.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cap_scheduler.started) {
        return ESP_OK;
    }

    s_cap_scheduler.stop_requested = false;
    ok = claw_task_create(&task_config, cap_scheduler_task, NULL, &s_cap_scheduler.task_handle);
    if (ok != pdPASS) {
        s_cap_scheduler.task_handle = NULL;
        return ESP_FAIL;
    }
    s_cap_scheduler.started = true;
    return ESP_OK;
}

esp_err_t cap_scheduler_stop(void)
{
    if (!s_cap_scheduler.started || !s_cap_scheduler.task_handle) {
        return ESP_OK;
    }
    s_cap_scheduler.stop_requested = true;
    while (s_cap_scheduler.task_handle) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

esp_err_t cap_scheduler_reload(void)
{
    esp_err_t err;

    if (!s_cap_scheduler.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    cap_scheduler_lock();
    err = cap_scheduler_load_from_disk_locked();
    cap_scheduler_unlock();
    return err;
}

esp_err_t cap_scheduler_add(const cap_scheduler_item_t *item)
{
    cap_scheduler_item_t normalized_item;
    ssize_t index;
    esp_err_t err;
    int64_t now_ms = cap_scheduler_now_ms();

    if (!s_cap_scheduler.initialized || !item) {
        return ESP_ERR_INVALID_STATE;
    }

    normalized_item = *item;
    cap_scheduler_apply_defaults(&normalized_item);
    if (cap_scheduler_validate_item(&normalized_item) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    cap_scheduler_lock();
    if (cap_scheduler_find_entry_index_locked(normalized_item.id) >= 0) {
        cap_scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    index = cap_scheduler_find_free_index_locked();
    if (index < 0) {
        cap_scheduler_unlock();
        return ESP_ERR_NO_MEM;
    }
    memset(&s_cap_scheduler.entries[index], 0, sizeof(s_cap_scheduler.entries[index]));
    s_cap_scheduler.entries[index].occupied = true;
    s_cap_scheduler.entries[index].item = normalized_item;
    s_cap_scheduler.entries[index].status = normalized_item.enabled ?
                                            CAP_SCHEDULER_STATUS_SCHEDULED : CAP_SCHEDULER_STATUS_DISABLED;
    cap_scheduler_refresh_entry_locked(&s_cap_scheduler.entries[index], now_ms);
    s_cap_scheduler.item_count = cap_scheduler_active_count_locked();
    err = cap_scheduler_persist_all_locked();
    if (err != ESP_OK) {
        memset(&s_cap_scheduler.entries[index], 0, sizeof(s_cap_scheduler.entries[index]));
        s_cap_scheduler.item_count = cap_scheduler_active_count_locked();
        cap_scheduler_unlock();
        return err;
    }
    cap_scheduler_unlock();
    return ESP_OK;
}

esp_err_t cap_scheduler_update(const cap_scheduler_item_t *item)
{
    cap_scheduler_entry_t *previous_entry = NULL;
    cap_scheduler_item_t *normalized_item = NULL;
    ssize_t index;
    esp_err_t err;
    int64_t now_ms = cap_scheduler_now_ms();

    if (!s_cap_scheduler.initialized || !item) {
        return ESP_ERR_INVALID_STATE;
    }

    normalized_item = calloc(1, sizeof(*normalized_item));
    previous_entry = calloc(1, sizeof(*previous_entry));
    if (!normalized_item || !previous_entry) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *normalized_item = *item;
    cap_scheduler_apply_defaults(normalized_item);
    if (cap_scheduler_validate_item(normalized_item) != ESP_OK) {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    cap_scheduler_lock();
    index = cap_scheduler_find_entry_index_locked(normalized_item->id);
    if (index < 0) {
        cap_scheduler_unlock();
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    *previous_entry = s_cap_scheduler.entries[index];
    s_cap_scheduler.entries[index].item = *normalized_item;
    if (s_cap_scheduler.entries[index].status != CAP_SCHEDULER_STATUS_PAUSED) {
        cap_scheduler_refresh_entry_locked(&s_cap_scheduler.entries[index], now_ms);
    }
    err = cap_scheduler_persist_all_locked();
    if (err != ESP_OK) {
        s_cap_scheduler.entries[index] = *previous_entry;
        cap_scheduler_unlock();
        goto cleanup;
    }
    cap_scheduler_unlock();
    err = ESP_OK;

cleanup:
    free(previous_entry);
    free(normalized_item);
    return err;
}

esp_err_t cap_scheduler_remove(const char *id)
{
    cap_scheduler_entry_t previous_entry;
    ssize_t index;
    esp_err_t err;

    if (!s_cap_scheduler.initialized || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    cap_scheduler_lock();
    index = cap_scheduler_find_entry_index_locked(id);
    if (index < 0) {
        cap_scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    previous_entry = s_cap_scheduler.entries[index];
    memset(&s_cap_scheduler.entries[index], 0, sizeof(s_cap_scheduler.entries[index]));
    s_cap_scheduler.item_count = cap_scheduler_active_count_locked();
    err = cap_scheduler_persist_all_locked();
    if (err != ESP_OK) {
        s_cap_scheduler.entries[index] = previous_entry;
        s_cap_scheduler.item_count = cap_scheduler_active_count_locked();
        cap_scheduler_unlock();
        return err;
    }
    cap_scheduler_unlock();
    return ESP_OK;
}

esp_err_t cap_scheduler_enable(const char *id, bool enabled)
{
    ssize_t index;
    cap_scheduler_entry_t previous_entry;
    esp_err_t err;
    int64_t now_ms = cap_scheduler_now_ms();

    if (!s_cap_scheduler.initialized || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    cap_scheduler_lock();
    index = cap_scheduler_find_entry_index_locked(id);
    if (index < 0) {
        cap_scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    previous_entry = s_cap_scheduler.entries[index];
    s_cap_scheduler.entries[index].item.enabled = enabled;
    s_cap_scheduler.entries[index].status = enabled ?
                                            CAP_SCHEDULER_STATUS_SCHEDULED : CAP_SCHEDULER_STATUS_DISABLED;
    s_cap_scheduler.entries[index].last_error_code = ESP_OK;
    cap_scheduler_refresh_entry_locked(&s_cap_scheduler.entries[index], now_ms);
    err = cap_scheduler_persist_all_locked();
    if (err != ESP_OK) {
        s_cap_scheduler.entries[index] = previous_entry;
        cap_scheduler_unlock();
        return err;
    }
    cap_scheduler_unlock();
    return ESP_OK;
}

esp_err_t cap_scheduler_pause(const char *id)
{
    cap_scheduler_entry_t previous_entry;
    ssize_t index;
    esp_err_t err;

    if (!s_cap_scheduler.initialized || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    cap_scheduler_lock();
    index = cap_scheduler_find_entry_index_locked(id);
    if (index < 0) {
        cap_scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    previous_entry = s_cap_scheduler.entries[index];
    s_cap_scheduler.entries[index].status = CAP_SCHEDULER_STATUS_PAUSED;
    err = cap_scheduler_persist_runtime_state_locked();
    if (err != ESP_OK) {
        s_cap_scheduler.entries[index] = previous_entry;
        cap_scheduler_unlock();
        return err;
    }
    cap_scheduler_unlock();
    return ESP_OK;
}

esp_err_t cap_scheduler_resume(const char *id)
{
    ssize_t index;
    cap_scheduler_entry_t previous_entry;
    esp_err_t err;
    int64_t now_ms = cap_scheduler_now_ms();

    if (!s_cap_scheduler.initialized || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    cap_scheduler_lock();
    index = cap_scheduler_find_entry_index_locked(id);
    if (index < 0) {
        cap_scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    previous_entry = s_cap_scheduler.entries[index];
    cap_scheduler_refresh_entry_locked(&s_cap_scheduler.entries[index], now_ms);
    err = cap_scheduler_persist_runtime_state_locked();
    if (err != ESP_OK) {
        s_cap_scheduler.entries[index] = previous_entry;
        cap_scheduler_unlock();
        return err;
    }
    cap_scheduler_unlock();
    return ESP_OK;
}

esp_err_t cap_scheduler_trigger_now(const char *id)
{
    ssize_t index;
    esp_err_t err;
    esp_err_t persist_err;

    if (!s_cap_scheduler.initialized || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    cap_scheduler_lock();
    index = cap_scheduler_find_entry_index_locked(id);
    if (index < 0) {
        cap_scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    err = cap_scheduler_publish_entry_locked(&s_cap_scheduler.entries[index], true, cap_scheduler_now_ms());
    persist_err = cap_scheduler_persist_runtime_state_locked();
    cap_scheduler_unlock();
    if (err != ESP_OK) {
        return err;
    }
    if (persist_err != ESP_OK) {
        return persist_err;
    }
    return err;
}

esp_err_t cap_scheduler_get_snapshot(const char *id, cap_scheduler_snapshot_t *out)
{
    ssize_t index;

    if (!s_cap_scheduler.initialized || !id || !id[0] || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    cap_scheduler_lock();
    index = cap_scheduler_find_entry_index_locked(id);
    if (index < 0) {
        cap_scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    memset(out, 0, sizeof(*out));
    out->item = s_cap_scheduler.entries[index].item;
    out->status = s_cap_scheduler.entries[index].status;
    out->next_fire_ms = s_cap_scheduler.entries[index].next_fire_ms;
    out->last_fire_ms = s_cap_scheduler.entries[index].last_fire_ms;
    out->last_success_ms = s_cap_scheduler.entries[index].last_success_ms;
    out->run_count = s_cap_scheduler.entries[index].run_count;
    out->missed_count = s_cap_scheduler.entries[index].missed_count;
    out->last_error_code = s_cap_scheduler.entries[index].last_error_code;
    cap_scheduler_unlock();
    return ESP_OK;
}

esp_err_t cap_scheduler_list_json(char *buf, size_t size)
{
    cJSON *root = NULL;
    char *rendered = NULL;

    if (!s_cap_scheduler.initialized || !buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    root = cJSON_CreateArray();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cap_scheduler_lock();
    for (size_t i = 0; i < s_cap_scheduler.max_items; i++) {
        cJSON *obj = NULL;

        if (!s_cap_scheduler.entries[i].occupied) {
            continue;
        }
        if (cap_scheduler_entry_to_json(&s_cap_scheduler.entries[i], true, &obj) == ESP_OK) {
            cJSON_AddItemToArray(root, obj);
        }
    }
    cap_scheduler_unlock();

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(buf, rendered, size);
    free(rendered);
    return ESP_OK;
}

esp_err_t cap_scheduler_get_state_json(const char *id, char *buf, size_t size)
{
    cap_scheduler_snapshot_t *snapshot = NULL;
    esp_err_t err;

    snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_scheduler_get_snapshot(id, snapshot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "snapshot not found: %s", esp_err_to_name(err));
        free(snapshot);
        return err;
    }

    err = cap_scheduler_write_snapshot_json(snapshot, buf, size);
    free(snapshot);
    return err;
}

esp_err_t cap_scheduler_handle_time_sync(void)
{
    bool had_valid_time;
    int64_t now_ms;
    esp_err_t err;

    if (!s_cap_scheduler.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    cap_scheduler_lock();
    had_valid_time = s_cap_scheduler.time_valid;
    s_cap_scheduler.time_valid = true;
    now_ms = cap_scheduler_now_ms();

    for (size_t i = 0; i < s_cap_scheduler.max_items; i++) {
        cap_scheduler_entry_t *entry = &s_cap_scheduler.entries[i];

        if (!entry->occupied) {
            continue;
        }

        if (!had_valid_time &&
                entry->item.kind == CAP_SCHEDULER_ITEM_INTERVAL &&
                !cap_scheduler_item_requires_valid_time(&entry->item)) {
            entry->last_fire_ms = 0;
            entry->last_success_ms = 0;
            entry->next_fire_ms = -1;
        }
        if (entry->status == CAP_SCHEDULER_STATUS_PAUSED) {
            continue;
        }

        err = cap_scheduler_refresh_entry_locked(entry, now_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to refresh schedule %s after time sync: %s",
                     entry->item.id,
                     esp_err_to_name(err));
        }
    }

    err = cap_scheduler_persist_runtime_state_locked();
    cap_scheduler_unlock();
    return err;
}

static esp_err_t cap_scheduler_execute_list_impl(const char *input_json,
                                                 char *output,
                                                 size_t output_size)
{
    (void)input_json;
    return cap_scheduler_list_json(output, output_size);
}

static esp_err_t cap_scheduler_execute_get_impl(const char *input_json,
                                                char *output,
                                                size_t output_size)
{
    char id[CAP_SCHEDULER_ID_LEN] = {0};

    ESP_RETURN_ON_ERROR(cap_scheduler_parse_id_input(input_json, id, sizeof(id)),
                        TAG,
                        "scheduler id required");
    return cap_scheduler_get_state_json(id, output, output_size);
}

static esp_err_t cap_scheduler_execute_add_impl(const char *input_json,
                                                char *output,
                                                size_t output_size)
{
    cap_scheduler_item_t *item = NULL;
    esp_err_t err;

    item = calloc(1, sizeof(*item));
    if (!item) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_scheduler_parse_add_input(input_json, item);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scheduler add input invalid: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = cap_scheduler_add(item);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scheduler add failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = cap_scheduler_get_state_json(item->id, output, output_size);

cleanup:
    free(item);
    return err;
}

static esp_err_t cap_scheduler_execute_update_impl(const char *input_json,
                                                   char *output,
                                                   size_t output_size)
{
    cap_scheduler_item_t *item = NULL;
    esp_err_t err;

    item = calloc(1, sizeof(*item));
    if (!item) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_scheduler_parse_add_input(input_json, item);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scheduler update input invalid: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = cap_scheduler_update(item);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scheduler update failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = cap_scheduler_get_state_json(item->id, output, output_size);

cleanup:
    free(item);
    return err;
}

static esp_err_t cap_scheduler_execute_enable_impl(const char *input_json,
                                                   char *output,
                                                   size_t output_size)
{
    char id[CAP_SCHEDULER_ID_LEN] = {0};

    ESP_RETURN_ON_ERROR(cap_scheduler_parse_id_input(input_json, id, sizeof(id)), TAG, "id required");
    ESP_RETURN_ON_ERROR(cap_scheduler_enable(id, true), TAG, "enable failed");
    snprintf(output, output_size, "{\"ok\":true,\"id\":\"%s\",\"enabled\":true}", id);
    return ESP_OK;
}

static esp_err_t cap_scheduler_execute_disable_impl(const char *input_json,
                                                    char *output,
                                                    size_t output_size)
{
    char id[CAP_SCHEDULER_ID_LEN] = {0};

    ESP_RETURN_ON_ERROR(cap_scheduler_parse_id_input(input_json, id, sizeof(id)), TAG, "id required");
    ESP_RETURN_ON_ERROR(cap_scheduler_enable(id, false), TAG, "disable failed");
    snprintf(output, output_size, "{\"ok\":true,\"id\":\"%s\",\"enabled\":false}", id);
    return ESP_OK;
}

static esp_err_t cap_scheduler_execute_remove_impl(const char *input_json,
                                                   char *output,
                                                   size_t output_size)
{
    char id[CAP_SCHEDULER_ID_LEN] = {0};

    ESP_RETURN_ON_ERROR(cap_scheduler_parse_id_input(input_json, id, sizeof(id)), TAG, "id required");
    ESP_RETURN_ON_ERROR(cap_scheduler_remove(id), TAG, "remove failed");
    snprintf(output, output_size, "{\"ok\":true,\"id\":\"%s\",\"removed\":true}", id);
    return ESP_OK;
}

static esp_err_t cap_scheduler_execute_pause_impl(const char *input_json,
                                                  char *output,
                                                  size_t output_size)
{
    char id[CAP_SCHEDULER_ID_LEN] = {0};

    ESP_RETURN_ON_ERROR(cap_scheduler_parse_id_input(input_json, id, sizeof(id)), TAG, "id required");
    ESP_RETURN_ON_ERROR(cap_scheduler_pause(id), TAG, "pause failed");
    snprintf(output, output_size, "{\"ok\":true,\"id\":\"%s\",\"status\":\"paused\"}", id);
    return ESP_OK;
}

static esp_err_t cap_scheduler_execute_resume_impl(const char *input_json,
                                                   char *output,
                                                   size_t output_size)
{
    char id[CAP_SCHEDULER_ID_LEN] = {0};

    ESP_RETURN_ON_ERROR(cap_scheduler_parse_id_input(input_json, id, sizeof(id)), TAG, "id required");
    ESP_RETURN_ON_ERROR(cap_scheduler_resume(id), TAG, "resume failed");
    snprintf(output, output_size, "{\"ok\":true,\"id\":\"%s\",\"status\":\"scheduled\"}", id);
    return ESP_OK;
}

static esp_err_t cap_scheduler_execute_trigger_impl(const char *input_json,
                                                    char *output,
                                                    size_t output_size)
{
    char id[CAP_SCHEDULER_ID_LEN] = {0};

    ESP_RETURN_ON_ERROR(cap_scheduler_parse_id_input(input_json, id, sizeof(id)), TAG, "id required");
    ESP_RETURN_ON_ERROR(cap_scheduler_trigger_now(id), TAG, "trigger failed");
    snprintf(output, output_size, "{\"ok\":true,\"id\":\"%s\",\"triggered\":true}", id);
    return ESP_OK;
}

static esp_err_t cap_scheduler_execute_reload_impl(const char *input_json,
                                                   char *output,
                                                   size_t output_size)
{
    (void)input_json;
    ESP_RETURN_ON_ERROR(cap_scheduler_reload(), TAG, "reload failed");
    snprintf(output, output_size, "{\"ok\":true}");
    return ESP_OK;
}
