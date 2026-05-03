/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_memory_internal.h"
#include "claw_task.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "claw_memory";

#define CLAW_MEMORY_ASYNC_EXTRACT_QUEUE_LEN 4
#define CLAW_MEMORY_ASYNC_EXTRACT_STACK_SIZE (6 * 1024)
#define CLAW_MEMORY_ASYNC_EXTRACT_PRIORITY 5
#define CLAW_MEMORY_ASYNC_EXTRACT_SWEEP_TICKS pdMS_TO_TICKS(60000)

typedef struct claw_memory_pending_summary {
    char *session_id;
    char *summary_list;
    struct claw_memory_pending_summary *next;
} claw_memory_pending_summary_t;

typedef struct claw_memory_async_extract_job {
    uint32_t request_id;
    char *session_id;
    char *user_text;
    char *llm_text;
    claw_memory_message_intent_t message_intent;
    TickType_t created_ticks;
    TickType_t completed_ticks;
    esp_err_t result;
    bool completed;
    SemaphoreHandle_t done_sem;
    struct claw_memory_async_extract_job *next;
} claw_memory_async_extract_job_t;

typedef struct claw_memory_request_state {
    uint32_t request_id;
    bool manual_write;
    struct claw_memory_request_state *next;
} claw_memory_request_state_t;

typedef struct {
    bool enabled;
    QueueHandle_t queue;
    SemaphoreHandle_t lock;
    TaskHandle_t task_handle;
    claw_llm_runtime_t *runtime;
    claw_memory_async_extract_job_t *jobs;
} claw_memory_async_extract_state_t;

static claw_memory_pending_summary_t *s_pending_summaries = NULL;
static claw_memory_async_extract_state_t s_async_extract = {0};
static claw_memory_request_state_t *s_request_states = NULL;

typedef struct {
    long content_offset;
    size_t content_len;
    size_t escape_extra;
    bool assistant;
} claw_memory_session_history_entry_t;

static claw_memory_pending_summary_t *claw_memory_find_pending_summary(const char *session_id)
{
    claw_memory_pending_summary_t *node = s_pending_summaries;

    while (node) {
        if (node->session_id && strcmp(node->session_id, session_id) == 0) {
            return node;
        }
        node = node->next;
    }

    return NULL;
}

static esp_err_t claw_memory_pending_summary_append(const char *session_id, const char *summary_list)
{
    claw_memory_pending_summary_t *node = NULL;

    if (!session_id || !session_id[0] || !summary_list || !summary_list[0]) {
        return ESP_OK;
    }

    node = claw_memory_find_pending_summary(session_id);
    if (!node) {
        node = calloc(1, sizeof(*node));
        if (!node) {
            return ESP_ERR_NO_MEM;
        }
        node->session_id = dup_printf("%s", session_id);
        if (!node->session_id) {
            free(node);
            return ESP_ERR_NO_MEM;
        }
        node->next = s_pending_summaries;
        s_pending_summaries = node;
    }

    return line_list_merge_unique(&node->summary_list, summary_list);
}

static char *claw_memory_pending_summary_take_summary_list(const char *session_id)
{
    claw_memory_pending_summary_t *node = s_pending_summaries;
    claw_memory_pending_summary_t *prev = NULL;
    char *summary_list = NULL;

    if (!session_id || !session_id[0]) {
        return NULL;
    }

    while (node) {
        if (node->session_id && strcmp(node->session_id, session_id) == 0) {
            break;
        }
        prev = node;
        node = node->next;
    }
    if (!node) {
        return NULL;
    }

    summary_list = node->summary_list;
    node->summary_list = NULL;
    if (prev) {
        prev->next = node->next;
    } else {
        s_pending_summaries = node->next;
    }
    free(node->session_id);
    free(node);
    return summary_list;
}

static claw_memory_request_state_t *claw_memory_find_request_state(uint32_t request_id)
{
    claw_memory_request_state_t *node = s_request_states;

    while (node) {
        if (node->request_id == request_id) {
            return node;
        }
        node = node->next;
    }

    return NULL;
}

esp_err_t claw_memory_request_mark_manual_write(uint32_t request_id)
{
    claw_memory_request_state_t *node = NULL;

    if (request_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    node = claw_memory_find_request_state(request_id);
    if (!node) {
        node = calloc(1, sizeof(*node));
        if (!node) {
            return ESP_ERR_NO_MEM;
        }
        node->request_id = request_id;
        node->next = s_request_states;
        s_request_states = node;
    }

    node->manual_write = true;
    return ESP_OK;
}

static bool claw_memory_request_take_manual_write(uint32_t request_id)
{
    claw_memory_request_state_t *node = s_request_states;
    claw_memory_request_state_t *prev = NULL;
    bool manual_write = false;

    if (request_id == 0) {
        return false;
    }

    while (node) {
        if (node->request_id == request_id) {
            break;
        }
        prev = node;
        node = node->next;
    }
    if (!node) {
        return false;
    }

    manual_write = node->manual_write;
    if (prev) {
        prev->next = node->next;
    } else {
        s_request_states = node->next;
    }
    free(node);
    return manual_write;
}

static claw_memory_async_extract_job_t *claw_memory_async_extract_find_job_locked(uint32_t request_id)
{
    claw_memory_async_extract_job_t *job = s_async_extract.jobs;

    while (job) {
        if (job->request_id == request_id) {
            return job;
        }
        job = job->next;
    }
    return NULL;
}

static void claw_memory_async_extract_free_job(claw_memory_async_extract_job_t *job)
{
    if (!job) {
        return;
    }
    if (job->done_sem) {
        vSemaphoreDelete(job->done_sem);
    }
    free(job->session_id);
    free(job->user_text);
    free(job->llm_text);
    free(job);
}

static void claw_memory_async_extract_sweep_locked(TickType_t now_ticks)
{
    claw_memory_async_extract_job_t *job = s_async_extract.jobs;
    claw_memory_async_extract_job_t *prev = NULL;

    while (job) {
        claw_memory_async_extract_job_t *next = job->next;
        bool expired = job->completed &&
                       (now_ticks - job->completed_ticks) >= CLAW_MEMORY_ASYNC_EXTRACT_SWEEP_TICKS;

        if (expired) {
            if (prev) {
                prev->next = next;
            } else {
                s_async_extract.jobs = next;
            }
            claw_memory_async_extract_free_job(job);
        } else {
            prev = job;
        }
        job = next;
    }
}

static void claw_memory_async_extract_task(void *arg)
{
    (void)arg;

    while (true) {
        claw_memory_async_extract_job_t *job = NULL;
        char *llm_text = NULL;
        claw_memory_message_intent_t message_intent = CLAW_MEMORY_MESSAGE_INTENT_NONE;
        esp_err_t err;

        if (xQueueReceive(s_async_extract.queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!job) {
            continue;
        }

        err = claw_memory_auto_extract_prepare_with_runtime(s_async_extract.runtime,
                                                            job->user_text,
                                                            &message_intent,
                                                            &llm_text);

        if (s_async_extract.lock && xSemaphoreTake(s_async_extract.lock, portMAX_DELAY) == pdTRUE) {
            if (job->llm_text) {
                free(job->llm_text);
            }
            job->llm_text = llm_text;
            job->message_intent = message_intent;
            job->result = err;
            job->completed = true;
            job->completed_ticks = xTaskGetTickCount();
            xSemaphoreGive(s_async_extract.lock);
        } else {
            free(llm_text);
        }

        if (job->done_sem) {
            xSemaphoreGive(job->done_sem);
        }
    }
}

static char *claw_memory_async_extract_take_summary_list(const claw_core_request_t *request,
                                                        bool apply_result)
{
    claw_memory_async_extract_job_t *job = NULL;
    claw_memory_async_extract_job_t *prev = NULL;
    SemaphoreHandle_t done_sem = NULL;
    char *llm_text = NULL;
    char *summary_list = NULL;
    claw_memory_message_intent_t message_intent = CLAW_MEMORY_MESSAGE_INTENT_NONE;

    if (!request || !request->request_id || !s_async_extract.enabled || !s_async_extract.lock) {
        return NULL;
    }

    while (true) {
        if (xSemaphoreTake(s_async_extract.lock, portMAX_DELAY) != pdTRUE) {
            return NULL;
        }

        prev = NULL;
        job = s_async_extract.jobs;
        while (job) {
            if (job->request_id == request->request_id) {
                break;
            }
            prev = job;
            job = job->next;
        }

        if (!job) {
            claw_memory_async_extract_sweep_locked(xTaskGetTickCount());
            xSemaphoreGive(s_async_extract.lock);
            return NULL;
        }

        if (job->completed) {
            llm_text = job->llm_text;
            job->llm_text = NULL;
            message_intent = job->message_intent;
            if (prev) {
                prev->next = job->next;
            } else {
                s_async_extract.jobs = job->next;
            }
            xSemaphoreGive(s_async_extract.lock);
            claw_memory_async_extract_free_job(job);
            if (!apply_result) {
                free(llm_text);
                return NULL;
            }
            if (claw_memory_auto_extract_apply_result(llm_text,
                                                      message_intent,
                                                      &summary_list) != ESP_OK) {
                free(llm_text);
                free(summary_list);
                return NULL;
            }
            free(llm_text);
            return summary_list;
        }

        done_sem = job->done_sem;
        xSemaphoreGive(s_async_extract.lock);
        ESP_LOGI(TAG, "stage note provider waiting request=%" PRIu32, request->request_id);
        if (!done_sem || xSemaphoreTake(done_sem, portMAX_DELAY) != pdTRUE) {
            return NULL;
        }
    }
}

static void claw_memory_async_extract_deinit(void)
{
    claw_memory_async_extract_job_t *job = s_async_extract.jobs;

    s_async_extract.jobs = NULL;
    while (job) {
        claw_memory_async_extract_job_t *next = job->next;

        claw_memory_async_extract_free_job(job);
        job = next;
    }
    if (s_async_extract.task_handle) {
        claw_task_delete(s_async_extract.task_handle);
        s_async_extract.task_handle = NULL;
    }
    if (s_async_extract.queue) {
        vQueueDelete(s_async_extract.queue);
        s_async_extract.queue = NULL;
    }
    if (s_async_extract.lock) {
        vSemaphoreDelete(s_async_extract.lock);
        s_async_extract.lock = NULL;
    }
    if (s_async_extract.runtime) {
        claw_llm_runtime_deinit(s_async_extract.runtime);
        s_async_extract.runtime = NULL;
    }
    s_async_extract.enabled = false;
}

esp_err_t claw_memory_async_extract_init(const claw_memory_config_t *config)
{
    BaseType_t task_result;
    const claw_memory_llm_config_t *llm = NULL;
    char *error_message = NULL;
    esp_err_t err;

    claw_memory_async_extract_deinit();

    if (!config || !config->enable_async_extract_stage_note) {
        return ESP_OK;
    }
    llm = &config->llm;

    if (!llm->api_key || !llm->api_key[0] ||
        !llm->model || !llm->model[0] ||
        !llm->profile || !llm->profile[0]) {
        ESP_LOGI(TAG, "Async memory extract disabled: LLM config incomplete");
        return ESP_OK;
    }

    err = claw_llm_runtime_init(&s_async_extract.runtime,
                                &(claw_llm_runtime_config_t) {
                                    .api_key = llm->api_key,
                                    .backend_type = llm->backend_type,
                                    .profile = llm->profile,
                                    .model = llm->model,
                                    .base_url = llm->base_url,
                                    .auth_type = llm->auth_type,
                                    .timeout_ms = llm->timeout_ms,
                                    .max_tokens = llm->max_tokens,
                                    .image_max_bytes = llm->image_max_bytes,
                                },
                                &error_message);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to init async memory extract runtime: %s",
                 error_message ? error_message : esp_err_to_name(err));
        free(error_message);
        claw_memory_async_extract_deinit();
        return err;
    }
    free(error_message);

    s_async_extract.lock = xSemaphoreCreateMutex();
    s_async_extract.queue = xQueueCreate(CLAW_MEMORY_ASYNC_EXTRACT_QUEUE_LEN,
                                         sizeof(claw_memory_async_extract_job_t *));
    if (!s_async_extract.lock || !s_async_extract.queue) {
        claw_memory_async_extract_deinit();
        return ESP_ERR_NO_MEM;
    }

    task_result = claw_task_create(&(claw_task_config_t){
                                        .name = "claw_mem_extract",
                                        .stack_size = CLAW_MEMORY_ASYNC_EXTRACT_STACK_SIZE,
                                        .priority = CLAW_MEMORY_ASYNC_EXTRACT_PRIORITY,
                                        .core_id = tskNO_AFFINITY,
                                        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                                    },
                                    claw_memory_async_extract_task,
                                    NULL,
                                    &s_async_extract.task_handle);
    if (task_result != pdPASS) {
        claw_memory_async_extract_deinit();
        return ESP_FAIL;
    }

    s_async_extract.enabled = true;
    ESP_LOGI(TAG, "Async memory extract worker ready");
    return ESP_OK;
}

esp_err_t claw_memory_async_extract_ensure_started(const claw_core_request_t *request)
{
    claw_memory_async_extract_job_t *job = NULL;

    if (!request || !request->request_id || !request->session_id || !request->session_id[0] ||
        !request->user_text || !request->user_text[0] || !s_async_extract.enabled) {
        return ESP_OK;
    }
    if (!s_async_extract.lock || !s_async_extract.queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_async_extract.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    claw_memory_async_extract_sweep_locked(xTaskGetTickCount());
    if (claw_memory_async_extract_find_job_locked(request->request_id)) {
        xSemaphoreGive(s_async_extract.lock);
        return ESP_OK;
    }

    job = calloc(1, sizeof(*job));
    if (!job) {
        xSemaphoreGive(s_async_extract.lock);
        return ESP_ERR_NO_MEM;
    }

    job->request_id = request->request_id;
    job->session_id = dup_printf("%s", request->session_id);
    job->user_text = dup_printf("%s", request->user_text);
    job->done_sem = xSemaphoreCreateBinary();
    job->created_ticks = xTaskGetTickCount();
    if (!job->session_id || !job->user_text || !job->done_sem) {
        xSemaphoreGive(s_async_extract.lock);
        claw_memory_async_extract_free_job(job);
        return ESP_ERR_NO_MEM;
    }

    job->next = s_async_extract.jobs;
    s_async_extract.jobs = job;
    xSemaphoreGive(s_async_extract.lock);

    if (xQueueSend(s_async_extract.queue, &job, 0) != pdTRUE) {
        if (xSemaphoreTake(s_async_extract.lock, portMAX_DELAY) == pdTRUE) {
            claw_memory_async_extract_job_t *node = s_async_extract.jobs;
            claw_memory_async_extract_job_t *prev = NULL;

            while (node) {
                if (node == job) {
                    if (prev) {
                        prev->next = node->next;
                    } else {
                        s_async_extract.jobs = node->next;
                    }
                    break;
                }
                prev = node;
                node = node->next;
            }
            xSemaphoreGive(s_async_extract.lock);
        }
        claw_memory_async_extract_free_job(job);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG,
             "async extract job created request=%" PRIu32 " session=%s",
             request->request_id,
             request->session_id);
    return ESP_OK;
}

static size_t session_history_record_wrapper_len(bool assistant)
{
    static const char prefix[] = "{\"role\":\"";
    static const char middle[] = "\",\"content\":\"";
    static const char suffix[] = "\"}";
    const char *role = assistant ? "assistant" : "user";

    return (sizeof(prefix) - 1) + strlen(role) + (sizeof(middle) - 1) +
        (sizeof(suffix) - 1);
}

static esp_err_t session_history_read_entry(FILE *file,
                                            claw_memory_session_history_entry_t *entry,
                                            bool *out_valid,
                                            bool *out_eof)
{
    char role[16] = {0};
    size_t role_len = 0;
    int ch;
    bool saw_tab = false;

    if (!file || !entry || !out_valid || !out_eof) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(entry, 0, sizeof(*entry));
    *out_valid = false;
    *out_eof = false;

    ch = fgetc(file);
    if (ch == EOF) {
        if (ferror(file)) {
            ESP_LOGE(TAG, "read session history failed");
            return ESP_FAIL;
        }
        *out_eof = true;
        return ESP_OK;
    }

    while (ch != EOF) {
        if (ch == '\r' || ch == '\n') {
            if (ch == '\r') {
                int next = fgetc(file);

                if (next != '\n' && next != EOF) {
                    if (ungetc(next, file) == EOF) {
                        ESP_LOGE(TAG, "ungetc session history failed");
                        return ESP_FAIL;
                    }
                }
            }
            break;
        }

        if (!saw_tab) {
            if (ch == '\t') {
                long offset = ftell(file);

                if (offset < 0) {
                    ESP_LOGE(TAG, "ftell session history failed");
                    return ESP_FAIL;
                }
                saw_tab = true;
                entry->content_offset = offset;
            } else if (role_len + 1 < sizeof(role)) {
                role[role_len++] = (char)ch;
                role[role_len] = '\0';
            }
        } else {
            if (ch == '"' || ch == '\\') {
                entry->escape_extra++;
            }
            entry->content_len++;
        }

        ch = fgetc(file);
    }

    if (ferror(file)) {
        ESP_LOGE(TAG, "read session history failed");
        return ESP_FAIL;
    }

    if (saw_tab) {
        entry->assistant = strcmp(role, "assistant") == 0;
        *out_valid = true;
    }
    return ESP_OK;
}

static esp_err_t session_history_measure(FILE *file,
                                         claw_memory_session_history_entry_t *entries,
                                         size_t max_msgs,
                                         size_t *out_count,
                                         size_t *out_next,
                                         size_t *out_json_size)
{
    size_t count = 0;
    size_t next = 0;
    size_t json_size = 0;
    size_t i;
    esp_err_t err;

    if (!file || !entries || max_msgs == 0 || !out_count || !out_next || !out_json_size) {
        return ESP_ERR_INVALID_ARG;
    }

    while (true) {
        claw_memory_session_history_entry_t entry = {0};
        bool valid = false;
        bool eof = false;

        err = session_history_read_entry(file, &entry, &valid, &eof);
        if (err != ESP_OK) {
            return err;
        }
        if (eof) {
            break;
        }
        if (!valid) {
            continue;
        }

        entries[next] = entry;
        next = (next + 1) % max_msgs;
        if (count < max_msgs) {
            count++;
        }
    }

    if (count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    json_size = 3; /* '[' + ']' + trailing NUL */
    json_size += count - 1;
    for (i = 0; i < count; i++) {
        size_t idx = (count < max_msgs) ? i : ((next + i) % max_msgs);

        json_size += session_history_record_wrapper_len(entries[idx].assistant);
        json_size += entries[idx].content_len;
        json_size += entries[idx].escape_extra;
    }

    *out_count = count;
    *out_next = next;
    *out_json_size = json_size;
    return ESP_OK;
}

static void session_history_append_literal(char **cursor, const char *text)
{
    size_t len = strlen(text);

    memcpy(*cursor, text, len);
    *cursor += len;
}

static esp_err_t session_history_render_content(FILE *file,
                                                const claw_memory_session_history_entry_t *entry,
                                                char **cursor)
{
    size_t i;

    if (fseek(file, entry->content_offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "seek session history content failed");
        return ESP_FAIL;
    }

    for (i = 0; i < entry->content_len; i++) {
        int ch = fgetc(file);

        if (ch == EOF) {
            ESP_LOGE(TAG, "read session history content failed");
            return ESP_FAIL;
        }
        if (ch == '"' || ch == '\\') {
            *(*cursor)++ = '\\';
        }
        *(*cursor)++ = (char)ch;
    }
    return ESP_OK;
}

static esp_err_t session_history_render_json(FILE *file,
                                             const claw_memory_session_history_entry_t *entries,
                                             size_t max_msgs,
                                             size_t count,
                                             size_t next,
                                             char *json,
                                             size_t json_size)
{
    char *cursor = json;
    char *expected_end = json + json_size - 1;
    size_t i;

    if (!file || !entries || !json || json_size < 3 || count == 0 || max_msgs == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *cursor++ = '[';
    for (i = 0; i < count; i++) {
        size_t idx = (count < max_msgs) ? i : ((next + i) % max_msgs);
        const claw_memory_session_history_entry_t *entry = &entries[idx];
        const char *role = entry->assistant ? "assistant" : "user";
        esp_err_t err;

        if (i > 0) {
            *cursor++ = ',';
        }
        session_history_append_literal(&cursor, "{\"role\":\"");
        session_history_append_literal(&cursor, role);
        session_history_append_literal(&cursor, "\",\"content\":\"");
        err = session_history_render_content(file, entry, &cursor);
        if (err != ESP_OK) {
            return err;
        }
        session_history_append_literal(&cursor, "\"}");
    }
    *cursor++ = ']';
    *cursor = '\0';

    if (cursor != expected_end) {
        ESP_LOGE(TAG, "session history json size mismatch");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t session_history_close_file(FILE *file)
{
    if (!file) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "close session history failed: errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t claw_memory_session_load_json_alloc(const char *session_id, char **out_json)
{
    char *path = NULL;
    FILE *file = NULL;
    claw_memory_session_history_entry_t *entries = NULL;
    char *json = NULL;
    size_t max_msgs;
    size_t count = 0;
    size_t next = 0;
    size_t json_size = 0;
    esp_err_t err;

    if (!session_id || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_memory.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_json = NULL;
    max_msgs = s_memory.max_session_messages ? s_memory.max_session_messages :
        CLAW_MEMORY_DEFAULT_MAX_SESSION_MESSAGES;
    entries = calloc(max_msgs, sizeof(*entries));
    path = claw_memory_session_path_dup(session_id);
    if (!entries || !path) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    file = fopen(path, "r");
    if (!file) {
        err = (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "open session history %s failed: errno=%d", path, errno);
        }
        goto cleanup;
    }

    err = session_history_measure(file, entries, max_msgs, &count, &next, &json_size);
    if (err != ESP_OK) {
        goto cleanup;
    }

    // ESP_LOGI(TAG,
    //          "session history alloc session=%s retained=%u/%u metadata=%u json=%u heap_free=%u largest_block=%u",
    //          session_id,
    //          (unsigned)count,
    //          (unsigned)max_msgs,
    //          (unsigned)(max_msgs * sizeof(*entries)),
    //          (unsigned)json_size,
    //          (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
    //          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    json = calloc(1, json_size);
    if (!json) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = session_history_render_json(file, entries, max_msgs, count, next, json, json_size);

cleanup:
    if (file && session_history_close_file(file) != ESP_OK && err == ESP_OK) {
        err = ESP_FAIL;
    }
    free(entries);
    free(path);
    if (err != ESP_OK) {
        free(json);
        return err;
    }

    *out_json = json;
    return ESP_OK;
}

esp_err_t claw_memory_session_append(const char *session_id,
                                     const char *user_text,
                                     const char *assistant_text)
{
    char *path = NULL;
    FILE *file = NULL;
    esp_err_t err = ESP_OK;

    if (!session_id || !user_text || !assistant_text) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_memory.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    path = claw_memory_session_path_dup(session_id);
    if (!path) {
        return ESP_ERR_NO_MEM;
    }
    if (ensure_parent_dir(path) != ESP_OK) {
        free(path);
        return ESP_FAIL;
    }
    file = fopen(path, "a");
    if (!file) {
        free(path);
        return ESP_FAIL;
    }

    err = claw_memory_append_session_line(file, "user", user_text);
    if (err == ESP_OK) {
        err = claw_memory_append_session_line(file, "assistant", assistant_text);
    }
    fclose(file);
    free(path);
    return err;
}

esp_err_t claw_memory_note_session_summary(const char *session_id,
                                           const char *summary_list)
{
    return claw_memory_pending_summary_append(session_id, summary_list);
}

esp_err_t claw_memory_append_session_turn_callback(const char *session_id,
                                                   const char *user_text,
                                                   const char *assistant_text,
                                                   void *user_ctx)
{
    (void)user_ctx;
    return claw_memory_session_append(session_id, user_text, assistant_text);
}

esp_err_t claw_memory_request_start_callback(const claw_core_request_t *request,
                                             void *user_ctx)
{
    (void)user_ctx;
    return claw_memory_async_extract_ensure_started(request);
}

esp_err_t claw_memory_stage_note_callback(const claw_core_request_t *request,
                                          char **out_note,
                                          void *user_ctx)
{
    char *summary_list = NULL;
    char *async_summary = NULL;
    bool manual_write = false;

    (void)user_ctx;

    if (!out_note) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_note = NULL;
    if (!request || !request->session_id || !request->session_id[0]) {
        return ESP_OK;
    }

    manual_write = claw_memory_request_take_manual_write(request->request_id);
    summary_list = claw_memory_pending_summary_take_summary_list(request->session_id);
    async_summary = claw_memory_async_extract_take_summary_list(request, !manual_write);
    if (line_list_merge_unique(&summary_list, async_summary) != ESP_OK) {
        ESP_LOGW(TAG, "merge async extract summary failed for request=%" PRIu32, request->request_id);
    }
    free(async_summary);
    *out_note = claw_memory_format_update_stage_note(summary_list);
    free(summary_list);
    return ESP_OK;
}

static esp_err_t claw_memory_session_history_collect(const claw_core_request_t *request,
                                                     claw_core_context_t *out_context,
                                                     void *user_ctx)
{
    char *content = NULL;
    esp_err_t err;

    (void)user_ctx;

    if (!request || !out_context || !request->session_id || !request->session_id[0]) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(out_context, 0, sizeof(*out_context));

    err = claw_memory_session_load_json_alloc(request->session_id, &content);
    if (err != ESP_OK) {
        return err;
    }
    if (!content || !content[0] || strcmp(content, "[]") == 0) {
        free(content);
        return ESP_ERR_NOT_FOUND;
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_MESSAGES;
    out_context->content = content;
    return ESP_OK;
}

const claw_core_context_provider_t claw_memory_session_history_provider = {
    .name = "Session History",
    .collect = claw_memory_session_history_collect,
    .user_ctx = NULL,
};
