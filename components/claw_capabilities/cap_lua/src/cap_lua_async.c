/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_lua_internal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cap_lua_async";

typedef struct {
    bool used;
    cap_lua_job_status_t status;
    char job_id[CAP_LUA_JOB_ID_LEN];
    char name[CAP_LUA_JOB_NAME_MAX];
    char exclusive[CAP_LUA_JOB_EXCLUSIVE_MAX];
    char path[CAP_LUA_JOB_PATH_MAX];
    char *args_json;
    char *summary;
    char *log_buf;
    size_t log_size;
    size_t log_len;
    size_t log_start;
    uint64_t log_seq;
    bool log_truncated;
    uint32_t timeout_ms;
    time_t created_at;
    time_t started_at;
    time_t finished_at;
    TaskHandle_t task_handle;
    bool sync_waiter;
    /* Cooperative cancellation flag, polled from the Lua hook. */
    volatile bool stop_requested;
} cap_lua_job_record_t;

typedef struct {
    int slot;
    char job_id[CAP_LUA_JOB_ID_LEN];
    char path[CAP_LUA_JOB_PATH_MAX];
    char *args_json;
    uint32_t timeout_ms;
    volatile bool *stop_requested;
} cap_lua_job_ctx_t;

static SemaphoreHandle_t s_job_lock;
static cap_lua_job_record_t s_jobs[CAP_LUA_ASYNC_MAX_JOBS];
static SemaphoreHandle_t s_slot_terminal_sem[CAP_LUA_ASYNC_MAX_JOBS];
static size_t s_running_jobs;
static bool s_runner_started;

const char *cap_lua_job_status_name(cap_lua_job_status_t status)
{
    switch (status) {
    case CAP_LUA_JOB_QUEUED:  return "queued";
    case CAP_LUA_JOB_RUNNING: return "running";
    case CAP_LUA_JOB_DONE:    return "done";
    case CAP_LUA_JOB_FAILED:  return "failed";
    case CAP_LUA_JOB_TIMEOUT: return "timeout";
    case CAP_LUA_JOB_STOPPED: return "stopped";
    default:                  return "unknown";
    }
}

static bool cap_lua_status_is_terminal(cap_lua_job_status_t status)
{
    return status == CAP_LUA_JOB_DONE ||
           status == CAP_LUA_JOB_FAILED ||
           status == CAP_LUA_JOB_TIMEOUT ||
           status == CAP_LUA_JOB_STOPPED;
}

static bool cap_lua_job_status_matches(cap_lua_job_status_t status, const char *filter)
{
    if (!filter || !filter[0] || strcmp(filter, "all") == 0) {
        return true;
    }
    return strcmp(cap_lua_job_status_name(status), filter) == 0;
}

static void cap_lua_generate_job_id(char *job_id, size_t size)
{
    snprintf(job_id, size, "%08x", (unsigned)esp_random());
}

static int cap_lua_find_reusable_slot_locked(void)
{
    int oldest_terminal = -1;

    for (int i = 0; i < CAP_LUA_ASYNC_MAX_JOBS; i++) {
        if (!s_jobs[i].used) {
            return i;
        }
        if (cap_lua_status_is_terminal(s_jobs[i].status) && !s_jobs[i].sync_waiter) {
            if (oldest_terminal < 0 ||
                    s_jobs[i].finished_at < s_jobs[oldest_terminal].finished_at) {
                oldest_terminal = i;
            }
        }
    }

    return oldest_terminal;
}

static int cap_lua_find_slot_by_id_or_name_locked(const char *needle)
{
    /* Prefer exact id match; fall back to active-job name match. */
    if (!needle || !needle[0]) {
        return -1;
    }

    for (int i = 0; i < CAP_LUA_ASYNC_MAX_JOBS; i++) {
        if (s_jobs[i].used && strcmp(s_jobs[i].job_id, needle) == 0) {
            return i;
        }
    }
    for (int i = 0; i < CAP_LUA_ASYNC_MAX_JOBS; i++) {
        if (s_jobs[i].used &&
                !cap_lua_status_is_terminal(s_jobs[i].status) &&
                s_jobs[i].name[0] &&
                strcmp(s_jobs[i].name, needle) == 0) {
            return i;
        }
    }
    return -1;
}

static int cap_lua_find_active_by_name_locked(const char *name)
{
    if (!name || !name[0]) {
        return -1;
    }
    for (int i = 0; i < CAP_LUA_ASYNC_MAX_JOBS; i++) {
        if (s_jobs[i].used &&
                !cap_lua_status_is_terminal(s_jobs[i].status) &&
                s_jobs[i].name[0] &&
                strcmp(s_jobs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int cap_lua_find_active_by_exclusive_locked(const char *exclusive)
{
    if (!exclusive || !exclusive[0]) {
        return -1;
    }
    for (int i = 0; i < CAP_LUA_ASYNC_MAX_JOBS; i++) {
        if (s_jobs[i].used &&
                !cap_lua_status_is_terminal(s_jobs[i].status) &&
                s_jobs[i].exclusive[0] &&
                strcmp(s_jobs[i].exclusive, exclusive) == 0) {
            return i;
        }
    }
    return -1;
}

static void cap_lua_clear_slot(cap_lua_job_record_t *job)
{
    if (!job) {
        return;
    }
    free(job->args_json);
    free(job->summary);
    free(job->log_buf);
    memset(job, 0, sizeof(*job));
}

static void cap_lua_log_copy_into_ring(char *ring,
                                       size_t ring_size,
                                       size_t start,
                                       const char *text,
                                       size_t len)
{
    size_t first;

    if (!ring || ring_size == 0 || !text || len == 0) {
        return;
    }

    first = ring_size - start;
    if (first > len) {
        first = len;
    }
    memcpy(ring + start, text, first);
    if (first < len) {
        memcpy(ring, text + first, len - first);
    }
}

static void cap_lua_async_append_log_locked(cap_lua_job_record_t *job,
                                            const char *text,
                                            size_t len)
{
    size_t original_len = len;
    size_t overflow;
    size_t write_pos;

    if (!job || !job->log_buf || job->log_size == 0 || !text || len == 0) {
        return;
    }

    if (len >= job->log_size) {
        text += len - job->log_size;
        len = job->log_size;
        job->log_start = 0;
        job->log_len = job->log_size;
        job->log_truncated = true;
        memcpy(job->log_buf, text, len);
        job->log_seq += original_len;
        return;
    }

    if (job->log_len + len > job->log_size) {
        overflow = job->log_len + len - job->log_size;
        job->log_start = (job->log_start + overflow) % job->log_size;
        job->log_len -= overflow;
        job->log_truncated = true;
    }

    write_pos = (job->log_start + job->log_len) % job->log_size;
    cap_lua_log_copy_into_ring(job->log_buf, job->log_size, write_pos, text, len);
    job->log_len += len;
    job->log_seq += len;
}

static void cap_lua_async_log_callback(void *user_ctx, const char *text, size_t len)
{
    cap_lua_job_ctx_t *ctx = (cap_lua_job_ctx_t *)user_ctx;

    if (!ctx || !text || len == 0 || !s_job_lock) {
        return;
    }
    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (ctx->slot >= 0 &&
            ctx->slot < CAP_LUA_ASYNC_MAX_JOBS &&
            s_jobs[ctx->slot].used &&
            strcmp(s_jobs[ctx->slot].job_id, ctx->job_id) == 0) {
        cap_lua_async_append_log_locked(&s_jobs[ctx->slot], text, len);
    }
    xSemaphoreGive(s_job_lock);
}

static size_t cap_lua_async_copy_log_range_locked(const cap_lua_job_record_t *job,
                                                  uint64_t from_seq,
                                                  size_t max_bytes,
                                                  char *out,
                                                  size_t out_size,
                                                  uint64_t *actual_from,
                                                  uint64_t *next_seq,
                                                  bool *truncated)
{
    uint64_t earliest;
    uint64_t available;
    size_t copy_len;
    size_t offset;
    size_t src_pos;
    size_t first;

    if (actual_from) {
        *actual_from = 0;
    }
    if (next_seq) {
        *next_seq = 0;
    }
    if (truncated) {
        *truncated = false;
    }
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    if (!job || !job->log_buf || job->log_size == 0 || job->log_len == 0) {
        if (actual_from) {
            *actual_from = job ? job->log_seq : 0;
        }
        if (next_seq) {
            *next_seq = job ? job->log_seq : 0;
        }
        return 0;
    }

    earliest = job->log_seq - job->log_len;
    if (from_seq < earliest) {
        from_seq = earliest;
        if (truncated) {
            *truncated = true;
        }
    }
    if (from_seq > job->log_seq) {
        from_seq = job->log_seq;
    }

    available = job->log_seq - from_seq;
    copy_len = available > SIZE_MAX ? SIZE_MAX : (size_t)available;
    if (copy_len > max_bytes) {
        copy_len = max_bytes;
    }
    if (copy_len >= out_size) {
        copy_len = out_size - 1;
    }

    offset = (size_t)(from_seq - earliest);
    src_pos = (job->log_start + offset) % job->log_size;
    first = job->log_size - src_pos;
    if (first > copy_len) {
        first = copy_len;
    }
    memcpy(out, job->log_buf + src_pos, first);
    if (first < copy_len) {
        memcpy(out + first, job->log_buf, copy_len - first);
    }
    out[copy_len] = '\0';

    if (actual_from) {
        *actual_from = from_seq;
    }
    if (next_seq) {
        *next_seq = from_seq + copy_len;
    }
    return copy_len;
}

static void cap_lua_finish_job(cap_lua_job_ctx_t *ctx,
                               bool ok,
                               const char *error_msg,
                               const char *summary)
{
    bool stopped = false;
    bool timed_out = false;
    bool became_terminal = false;

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    if (ctx->slot >= 0 &&
            ctx->slot < CAP_LUA_ASYNC_MAX_JOBS &&
            s_jobs[ctx->slot].used &&
            strcmp(s_jobs[ctx->slot].job_id, ctx->job_id) == 0) {
        stopped = s_jobs[ctx->slot].stop_requested;
        if (!stopped && error_msg && strstr(error_msg, "execution timed out") != NULL) {
            timed_out = true;
        }
        if (stopped) {
            s_jobs[ctx->slot].status = CAP_LUA_JOB_STOPPED;
        } else if (timed_out) {
            s_jobs[ctx->slot].status = CAP_LUA_JOB_TIMEOUT;
        } else {
            s_jobs[ctx->slot].status = ok ? CAP_LUA_JOB_DONE : CAP_LUA_JOB_FAILED;
        }
        s_jobs[ctx->slot].finished_at = time(NULL);
        s_jobs[ctx->slot].task_handle = NULL;
        if (summary && summary[0]) {
            free(s_jobs[ctx->slot].summary);
            s_jobs[ctx->slot].summary = strdup(summary);
        }
        became_terminal = true;
    }

    if (s_running_jobs > 0) {
        s_running_jobs--;
    }

    xSemaphoreGive(s_job_lock);

    if (became_terminal && s_slot_terminal_sem[ctx->slot]) {
        xSemaphoreGive(s_slot_terminal_sem[ctx->slot]);
    }
}

static void cap_lua_job_task(void *arg)
{
    cap_lua_job_ctx_t *ctx = (cap_lua_job_ctx_t *)arg;
    char *output = NULL;
    esp_err_t err;

    if (!ctx) {
        claw_task_delete(NULL);
        return;
    }

    output = calloc(1, CAP_LUA_OUTPUT_SIZE);
    if (!output) {
        cap_lua_finish_job(ctx, false, NULL, "failed to allocate output buffer");
        free(ctx->args_json);
        free(ctx);
        claw_task_delete(NULL);
        return;
    }

    err = cap_lua_runtime_execute_file(ctx->path,
                                       ctx->args_json,
                                       ctx->timeout_ms,
                                       ctx->stop_requested,
                                       cap_lua_async_log_callback,
                                       ctx,
                                       output,
                                       CAP_LUA_OUTPUT_SIZE);
    cap_lua_finish_job(ctx, err == ESP_OK, output, output);
    free(output);
    free(ctx->args_json);
    free(ctx);
    claw_task_delete(NULL);
}

esp_err_t cap_lua_async_init(void)
{
    if (!s_job_lock) {
        s_job_lock = xSemaphoreCreateMutex();
    }
    if (!s_job_lock) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < CAP_LUA_ASYNC_MAX_JOBS; i++) {
        cap_lua_clear_slot(&s_jobs[i]);
        if (!s_slot_terminal_sem[i]) {
            s_slot_terminal_sem[i] = xSemaphoreCreateBinary();
            if (!s_slot_terminal_sem[i]) {
                return ESP_ERR_NO_MEM;
            }
        }
    }
    memset(s_jobs, 0, sizeof(s_jobs));
    s_running_jobs = 0;
    s_runner_started = false;
    return ESP_OK;
}

esp_err_t cap_lua_async_start(void)
{
    if (!s_job_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    s_runner_started = true;
    return ESP_OK;
}

static void cap_lua_format_active_jobs_locked(char *out, size_t size)
{
    size_t off = 0;
    int shown = 0;

    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';

    for (int i = 0; i < CAP_LUA_ASYNC_MAX_JOBS && off < size - 1; i++) {
        if (!s_jobs[i].used || cap_lua_status_is_terminal(s_jobs[i].status)) {
            continue;
        }
        int written = snprintf(out + off, size - off,
                               "%s%s(id=%s,exclusive=%s)",
                               shown == 0 ? "" : ", ",
                               s_jobs[i].name[0] ? s_jobs[i].name : "(unnamed)",
                               s_jobs[i].job_id,
                               s_jobs[i].exclusive[0] ? s_jobs[i].exclusive : "none");
        if (written < 0 || (size_t)written >= size - off) {
            break;
        }
        off += (size_t)written;
        shown++;
    }
    if (shown == 0) {
        snprintf(out, size, "(none)");
    }
}

/* Wait for a slot to leave RUNNING/QUEUED. Returns true if terminal in time. */
static bool cap_lua_slot_is_terminal(int slot)
{
    bool terminal = false;
    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    if (slot >= 0 && slot < CAP_LUA_ASYNC_MAX_JOBS &&
            (!s_jobs[slot].used || cap_lua_status_is_terminal(s_jobs[slot].status))) {
        terminal = true;
    }
    xSemaphoreGive(s_job_lock);
    return terminal;
}

static bool cap_lua_wait_for_terminal(int slot, uint32_t wait_ms)
{
    if (slot < 0 || slot >= CAP_LUA_ASYNC_MAX_JOBS) {
        return false;
    }
    if (cap_lua_slot_is_terminal(slot)) {
        return true;
    }
    SemaphoreHandle_t sem = s_slot_terminal_sem[slot];
    if (!sem) {
        return false;
    }
    xSemaphoreTake(sem, pdMS_TO_TICKS(wait_ms));
    return cap_lua_slot_is_terminal(slot);
}

static esp_err_t cap_lua_stop_slot_and_wait(int slot,
                                            const char *expected_job_id,
                                            uint32_t wait_ms,
                                            bool *out_was_running)
{
    bool was_running = false;

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (slot < 0 || slot >= CAP_LUA_ASYNC_MAX_JOBS || !s_jobs[slot].used) {
        xSemaphoreGive(s_job_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_job_id && expected_job_id[0] &&
            strncmp(s_jobs[slot].job_id, expected_job_id,
                    sizeof(s_jobs[slot].job_id)) != 0) {
        xSemaphoreGive(s_job_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (!cap_lua_status_is_terminal(s_jobs[slot].status)) {
        s_jobs[slot].stop_requested = true;
        was_running = true;
        ESP_LOGI(TAG, "Stop requested for job %s (name=%s)",
                 s_jobs[slot].job_id,
                 s_jobs[slot].name[0] ? s_jobs[slot].name : "(unnamed)");
    }
    xSemaphoreGive(s_job_lock);

    if (out_was_running) {
        *out_was_running = was_running;
    }
    if (!was_running) {
        return ESP_OK;
    }

    bool terminal = cap_lua_wait_for_terminal(slot, wait_ms);
    if (!terminal) {
        ESP_LOGW(TAG, "Stop wait timed out for slot %d after %u ms; job may still unwind",
                 slot, (unsigned)wait_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t cap_lua_async_submit_once(const cap_lua_async_job_t *job,
                                           char *job_id_out,
                                           size_t job_id_out_size,
                                           char *err_out,
                                           size_t err_out_size,
                                           bool *out_recheck_lost_race);

esp_err_t cap_lua_async_submit(const cap_lua_async_job_t *job,
                               char *job_id_out,
                               size_t job_id_out_size,
                               char *err_out,
                               size_t err_out_size)
{
    bool retry = false;
    esp_err_t err = cap_lua_async_submit_once(job, job_id_out, job_id_out_size,
                                              err_out, err_out_size, &retry);
    if (err == ESP_ERR_INVALID_STATE && retry) {
        ESP_LOGI(TAG, "Recheck race lost; retrying submit once");
        vTaskDelay(pdMS_TO_TICKS(20));
        err = cap_lua_async_submit_once(job, job_id_out, job_id_out_size,
                                        err_out, err_out_size, NULL);
    }
    return err;
}

static esp_err_t cap_lua_async_copy_terminal_result_locked(int slot,
                                                           const char *expected_job_id,
                                                           cap_lua_job_status_t *out_status,
                                                           char *output,
                                                           size_t output_size)
{
    const char *summary = NULL;

    if (slot < 0 || slot >= CAP_LUA_ASYNC_MAX_JOBS || !expected_job_id ||
            !expected_job_id[0] || !out_status || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';
    if (!s_jobs[slot].used ||
            strncmp(s_jobs[slot].job_id, expected_job_id,
                    sizeof(s_jobs[slot].job_id)) != 0) {
        snprintf(output, output_size, "Error: Lua sync job disappeared: %s", expected_job_id);
        return ESP_ERR_NOT_FOUND;
    }

    *out_status = s_jobs[slot].status;
    summary = s_jobs[slot].summary;
    if (summary && summary[0]) {
        strlcpy(output, summary, output_size);
    } else if (cap_lua_status_is_terminal(s_jobs[slot].status)) {
        snprintf(output, output_size, "Lua script completed with no output.\n");
    }
    return ESP_OK;
}

esp_err_t cap_lua_async_run_and_wait(const cap_lua_async_job_t *job,
                                     uint32_t wait_ms,
                                     char *output,
                                     size_t output_size)
{
    char job_id[CAP_LUA_JOB_ID_LEN] = {0};
    char err_buf[256] = {0};
    int slot = -1;
    cap_lua_job_status_t status = CAP_LUA_JOB_QUEUED;
    esp_err_t err;

    if (!job || !job->path[0] || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    err = cap_lua_async_submit(job, job_id, sizeof(job_id), err_buf, sizeof(err_buf));
    if (err != ESP_OK) {
        if (err_buf[0]) {
            snprintf(output, output_size, "Error: %s", err_buf);
        } else {
            snprintf(output, output_size, "Error: failed to queue Lua sync job (%s)",
                     esp_err_to_name(err));
        }
        return err;
    }

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        snprintf(output, output_size, "Error: timed out while locating Lua sync job %s", job_id);
        return ESP_ERR_TIMEOUT;
    }
    slot = cap_lua_find_slot_by_id_or_name_locked(job_id);
    if (slot < 0) {
        xSemaphoreGive(s_job_lock);
        snprintf(output, output_size, "Error: Lua sync job not found after submit: %s", job_id);
        return ESP_ERR_NOT_FOUND;
    }
    status = s_jobs[slot].status;
    bool already_terminal = cap_lua_status_is_terminal(status);
    xSemaphoreGive(s_job_lock);

    if (!already_terminal) {
        cap_lua_wait_for_terminal(slot, wait_ms);
    }

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        snprintf(output, output_size, "Error: timed out while collecting Lua sync job %s", job_id);
        return ESP_ERR_TIMEOUT;
    }
    err = cap_lua_async_copy_terminal_result_locked(slot, job_id, &status, output, output_size);
    bool terminal = (err == ESP_OK) && cap_lua_status_is_terminal(status);
    xSemaphoreGive(s_job_lock);

    if (!terminal) {
        bool was_running = false;
        (void)cap_lua_stop_slot_and_wait(slot, job_id, 0, &was_running);
        if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s_jobs[slot].used &&
                    strncmp(s_jobs[slot].job_id, job_id,
                            sizeof(s_jobs[slot].job_id)) == 0) {
                s_jobs[slot].sync_waiter = false;
            }
            xSemaphoreGive(s_job_lock);
        }
        snprintf(output, output_size,
                 "Error: Lua script timed out after %u ms (job_id=%s%s)",
                 (unsigned)wait_ms,
                 job_id,
                 was_running ? ", stop requested" : "");
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (s_jobs[slot].used &&
                strncmp(s_jobs[slot].job_id, job_id,
                        sizeof(s_jobs[slot].job_id)) == 0) {
            cap_lua_clear_slot(&s_jobs[slot]);
        }
        xSemaphoreGive(s_job_lock);
    }

    switch (status) {
    case CAP_LUA_JOB_DONE:
        return ESP_OK;
    case CAP_LUA_JOB_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case CAP_LUA_JOB_STOPPED:
        return ESP_ERR_TIMEOUT;
    case CAP_LUA_JOB_FAILED:
    default:
        return ESP_FAIL;
    }
}

static esp_err_t cap_lua_async_submit_once(const cap_lua_async_job_t *job,
                                           char *job_id_out,
                                           size_t job_id_out_size,
                                           char *err_out,
                                           size_t err_out_size,
                                           bool *out_recheck_lost_race)
{
    size_t log_bytes;
    char *log_buf = NULL;

    if (out_recheck_lost_race) {
        *out_recheck_lost_race = false;
    }
    if (!job || !job->path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_job_lock || !s_runner_started) {
        return ESP_ERR_INVALID_STATE;
    }
    log_bytes = job->log_bytes ? job->log_bytes : CAP_LUA_ASYNC_LOG_DEFAULT_BYTES;
    if (log_bytes < CAP_LUA_ASYNC_LOG_MIN_BYTES || log_bytes > CAP_LUA_ASYNC_LOG_MAX_BYTES) {
        if (err_out && err_out_size > 0) {
            snprintf(err_out, err_out_size,
                     "log_bytes must be between %u and %u",
                     (unsigned)CAP_LUA_ASYNC_LOG_MIN_BYTES,
                     (unsigned)CAP_LUA_ASYNC_LOG_MAX_BYTES);
        }
        return ESP_ERR_INVALID_ARG;
    }

    while (true) {
        int conflict_slot = -1;
        char conflict_reason[64] = {0};
        char conflict_id[CAP_LUA_JOB_ID_LEN] = {0};
        char conflict_name[CAP_LUA_JOB_NAME_MAX] = {0};
        char active_dump[256] = {0};
        bool over_concurrency = false;

        if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }

        if (job->name[0]) {
            int s = cap_lua_find_active_by_name_locked(job->name);
            if (s >= 0) {
                conflict_slot = s;
                snprintf(conflict_reason, sizeof(conflict_reason), "name '%s'", job->name);
            }
        }
        if (conflict_slot < 0 && job->exclusive[0]) {
            int s = cap_lua_find_active_by_exclusive_locked(job->exclusive);
            if (s >= 0) {
                conflict_slot = s;
                snprintf(conflict_reason, sizeof(conflict_reason),
                         "exclusive group '%s'", job->exclusive);
            }
        }
        if (conflict_slot >= 0) {
            strlcpy(conflict_id, s_jobs[conflict_slot].job_id, sizeof(conflict_id));
            strlcpy(conflict_name, s_jobs[conflict_slot].name, sizeof(conflict_name));
        } else if (s_running_jobs >= CAP_LUA_ASYNC_MAX_CONCURRENT) {
            over_concurrency = true;
            cap_lua_format_active_jobs_locked(active_dump, sizeof(active_dump));
        }

        xSemaphoreGive(s_job_lock);

        if (conflict_slot < 0 && !over_concurrency) {
            break; /* clear to submit */
        }

        if (over_concurrency) {
            if (err_out && err_out_size > 0) {
                snprintf(err_out, err_out_size,
                         "Concurrency limit reached (%u/%u). Running: %s. "
                         "Stop one with lua_stop_async_job before retrying.",
                         (unsigned)CAP_LUA_ASYNC_MAX_CONCURRENT,
                         (unsigned)CAP_LUA_ASYNC_MAX_CONCURRENT,
                         active_dump);
            }
            return ESP_ERR_NO_MEM;
        }

        if (!job->replace) {
            if (err_out && err_out_size > 0) {
                snprintf(err_out, err_out_size,
                         "Conflict with %s held by job '%s' (id=%s). "
                         "Pass replace=true to take over, or stop it first.",
                         conflict_reason,
                         conflict_name[0] ? conflict_name : "(unnamed)",
                         conflict_id);
            }
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Replacing conflicting job '%s' (id=%s) due to %s",
                 conflict_name[0] ? conflict_name : "(unnamed)",
                 conflict_id, conflict_reason);
        bool was_running = false;
        esp_err_t stop_err = cap_lua_stop_slot_and_wait(conflict_slot,
                                                        conflict_id,
                                                        CAP_LUA_STOP_WAIT_DEFAULT_MS,
                                                        &was_running);
        if (stop_err == ESP_ERR_TIMEOUT) {
            if (err_out && err_out_size > 0) {
                snprintf(err_out, err_out_size,
                         "Conflicting job '%s' (id=%s) did not stop within %u ms; "
                         "try again or stop it manually.",
                         conflict_name[0] ? conflict_name : "(unnamed)",
                         conflict_id,
                         (unsigned)CAP_LUA_STOP_WAIT_DEFAULT_MS);
            }
            return ESP_ERR_TIMEOUT;
        }
        if (stop_err != ESP_OK && stop_err != ESP_ERR_NOT_FOUND) {
            if (err_out && err_out_size > 0) {
                snprintf(err_out, err_out_size,
                         "Failed to stop conflicting job '%s': %s",
                         conflict_name[0] ? conflict_name : "(unnamed)",
                         esp_err_to_name(stop_err));
            }
            return stop_err;
        }
    }

    cap_lua_job_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    cap_lua_generate_job_id(ctx->job_id, sizeof(ctx->job_id));
    strlcpy(ctx->path, job->path, sizeof(ctx->path));
    ctx->timeout_ms = job->timeout_ms;
    if (job->args_json) {
        ctx->args_json = strdup(job->args_json);
        if (!ctx->args_json) {
            free(ctx);
            return ESP_ERR_NO_MEM;
        }
    }
    log_buf = calloc(1, log_bytes);
    if (!log_buf) {
        free(ctx->args_json);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    char submitted_job_id[CAP_LUA_JOB_ID_LEN] = {0};
    char submitted_path[CAP_LUA_JOB_PATH_MAX] = {0};
    strlcpy(submitted_job_id, ctx->job_id, sizeof(submitted_job_id));
    strlcpy(submitted_path, ctx->path, sizeof(submitted_path));

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(log_buf);
        free(ctx->args_json);
        free(ctx);
        return ESP_ERR_TIMEOUT;
    }

    if (s_running_jobs >= CAP_LUA_ASYNC_MAX_CONCURRENT) {
        xSemaphoreGive(s_job_lock);
        free(log_buf);
        free(ctx->args_json);
        free(ctx);
        if (err_out && err_out_size > 0) {
            snprintf(err_out, err_out_size,
                     "Concurrency limit reached after pre-flight (%u/%u); please retry.",
                     (unsigned)CAP_LUA_ASYNC_MAX_CONCURRENT,
                     (unsigned)CAP_LUA_ASYNC_MAX_CONCURRENT);
        }
        return ESP_ERR_NO_MEM;
    }

    int recheck_conflict = -1;
    char recheck_reason[64] = {0};
    char recheck_id[CAP_LUA_JOB_ID_LEN] = {0};
    char recheck_name[CAP_LUA_JOB_NAME_MAX] = {0};
    if (job->name[0]) {
        int s = cap_lua_find_active_by_name_locked(job->name);
        if (s >= 0) {
            recheck_conflict = s;
            snprintf(recheck_reason, sizeof(recheck_reason), "name '%s'", job->name);
        }
    }
    if (recheck_conflict < 0 && job->exclusive[0]) {
        int s = cap_lua_find_active_by_exclusive_locked(job->exclusive);
        if (s >= 0) {
            recheck_conflict = s;
            snprintf(recheck_reason, sizeof(recheck_reason),
                     "exclusive group '%s'", job->exclusive);
        }
    }
    if (recheck_conflict >= 0) {
        strlcpy(recheck_id, s_jobs[recheck_conflict].job_id, sizeof(recheck_id));
        strlcpy(recheck_name, s_jobs[recheck_conflict].name, sizeof(recheck_name));
        xSemaphoreGive(s_job_lock);
        free(log_buf);
        free(ctx->args_json);
        free(ctx);
        if (out_recheck_lost_race) {
            *out_recheck_lost_race = true;
        }
        if (err_out && err_out_size > 0) {
            snprintf(err_out, err_out_size,
                     "Slot for %s was taken by job '%s' (id=%s) before submit committed.",
                     recheck_reason,
                     recheck_name[0] ? recheck_name : "(unnamed)",
                     recheck_id);
        }
        return ESP_ERR_INVALID_STATE;
    }

    int slot = cap_lua_find_reusable_slot_locked();
    if (slot < 0) {
        xSemaphoreGive(s_job_lock);
        free(log_buf);
        free(ctx->args_json);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    cap_lua_clear_slot(&s_jobs[slot]);
    if (s_slot_terminal_sem[slot]) {
        xSemaphoreTake(s_slot_terminal_sem[slot], 0);
    }
    s_jobs[slot].used = true;
    s_jobs[slot].status = CAP_LUA_JOB_QUEUED;
    s_jobs[slot].created_at = job->created_at ? job->created_at : time(NULL);
    strlcpy(s_jobs[slot].job_id, ctx->job_id, sizeof(s_jobs[slot].job_id));
    strlcpy(s_jobs[slot].path, ctx->path, sizeof(s_jobs[slot].path));
    if (job->name[0]) {
        strlcpy(s_jobs[slot].name, job->name, sizeof(s_jobs[slot].name));
    }
    if (job->exclusive[0]) {
        strlcpy(s_jobs[slot].exclusive, job->exclusive, sizeof(s_jobs[slot].exclusive));
    }
    if (ctx->args_json) {
        s_jobs[slot].args_json = strdup(ctx->args_json);
        if (!s_jobs[slot].args_json) {
            cap_lua_clear_slot(&s_jobs[slot]);
            xSemaphoreGive(s_job_lock);
            free(log_buf);
            free(ctx->args_json);
            free(ctx);
            return ESP_ERR_NO_MEM;
        }
    }
    s_jobs[slot].log_buf = log_buf;
    s_jobs[slot].log_size = log_bytes;
    log_buf = NULL;
    s_jobs[slot].timeout_ms = job->timeout_ms;
    s_jobs[slot].sync_waiter = job->sync_waiter;
    s_jobs[slot].stop_requested = false;
    ctx->slot = slot;
    ctx->stop_requested = &s_jobs[slot].stop_requested;
    s_running_jobs++;
    xSemaphoreGive(s_job_lock);

    {
        claw_task_config_t task_config = {
            .name = "cap_lua_async",
            .stack_size = 12 * 1024,
            .priority = 4,
            .core_id = tskNO_AFFINITY,
            .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
        };

        if (claw_task_create(&task_config, cap_lua_job_task, ctx, &s_jobs[slot].task_handle) != pdPASS) {
            size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

            ESP_LOGE(TAG, "Failed to spawn task (stack=%u, free=%u, largest_block=%u)",
                     (unsigned)task_config.stack_size,
                     (unsigned)free_bytes,
                     (unsigned)largest);
            if (err_out && err_out_size > 0) {
                snprintf(err_out, err_out_size,
                         "Out of task memory for Lua task stack "
                         "(need %u bytes, largest free block %u bytes, total free %u bytes). "
                         "Stop another job and retry.",
                         (unsigned)task_config.stack_size,
                         (unsigned)largest,
                         (unsigned)free_bytes);
            }
            if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
                cap_lua_clear_slot(&s_jobs[slot]);
                if (s_running_jobs > 0) {
                    s_running_jobs--;
                }
                xSemaphoreGive(s_job_lock);
            }
            free(log_buf);
            free(ctx->args_json);
            free(ctx);
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (s_jobs[slot].used &&
                strncmp(s_jobs[slot].job_id, submitted_job_id,
                        sizeof(s_jobs[slot].job_id)) == 0 &&
                s_jobs[slot].status == CAP_LUA_JOB_QUEUED) {
            s_jobs[slot].status = CAP_LUA_JOB_RUNNING;
            s_jobs[slot].started_at = time(NULL);
        }
        xSemaphoreGive(s_job_lock);
    }

    if (job_id_out && job_id_out_size > 0) {
        strlcpy(job_id_out, submitted_job_id, job_id_out_size);
    }

    ESP_LOGI(TAG, "Queued Lua async job %s name=%s exclusive=%s timeout_ms=%u log_bytes=%u path=%s",
             submitted_job_id,
             job->name[0] ? job->name : "(unnamed)",
             job->exclusive[0] ? job->exclusive : "none",
             (unsigned)job->timeout_ms,
             (unsigned)log_bytes,
             submitted_path);
    return ESP_OK;
}

static int cap_lua_format_runtime_seconds(time_t now, time_t since)
{
    if (since <= 0) {
        return 0;
    }
    long diff = (long)(now - since);
    return diff < 0 ? 0 : (int)diff;
}

esp_err_t cap_lua_async_list_jobs(const char *status_filter,
                                  char *output,
                                  size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    time_t now = time(NULL);
    size_t offset = 0;
    int shown = 0;

    for (int i = 0; i < CAP_LUA_ASYNC_MAX_JOBS && offset < output_size - 1; i++) {
        if (!s_jobs[i].used || !cap_lua_job_status_matches(s_jobs[i].status, status_filter)) {
            continue;
        }
        int runtime_s = cap_lua_format_runtime_seconds(
            now,
            s_jobs[i].started_at ? s_jobs[i].started_at : s_jobs[i].created_at);
        int written = snprintf(output + offset, output_size - offset,
                               "%s | %s | name=%s | exclusive=%s | runtime=%ds | path=%s\n",
                               s_jobs[i].job_id,
                               cap_lua_job_status_name(s_jobs[i].status),
                               s_jobs[i].name[0] ? s_jobs[i].name : "(unnamed)",
                               s_jobs[i].exclusive[0] ? s_jobs[i].exclusive : "none",
                               runtime_s,
                               s_jobs[i].path);
        if (written < 0 || (size_t)written >= output_size - offset) {
            break;
        }
        offset += (size_t)written;
        shown++;
    }

    xSemaphoreGive(s_job_lock);
    if (shown == 0) {
        snprintf(output, output_size, "(no Lua async jobs)");
    }
    return ESP_OK;
}

esp_err_t cap_lua_async_get_job(const char *id_or_name,
                                char *output,
                                size_t output_size)
{
    char *recent_log = NULL;
    const size_t recent_log_size = CAP_LUA_ASYNC_LOG_TAIL_DEFAULT_BYTES + 1;

    if (!id_or_name || !id_or_name[0] || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    recent_log = calloc(1, recent_log_size);
    if (!recent_log) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(recent_log);
        return ESP_ERR_TIMEOUT;
    }

    int slot = cap_lua_find_slot_by_id_or_name_locked(id_or_name);
    if (slot < 0) {
        xSemaphoreGive(s_job_lock);
        free(recent_log);
        snprintf(output, output_size, "Error: Lua async job not found: %s", id_or_name);
        return ESP_ERR_NOT_FOUND;
    }

    time_t now = time(NULL);
    int runtime_s = cap_lua_format_runtime_seconds(
        now,
        s_jobs[slot].started_at ? s_jobs[slot].started_at : s_jobs[slot].created_at);
    uint64_t log_from = s_jobs[slot].log_seq > CAP_LUA_ASYNC_LOG_TAIL_DEFAULT_BYTES
                        ? s_jobs[slot].log_seq - CAP_LUA_ASYNC_LOG_TAIL_DEFAULT_BYTES
                        : 0;
    uint64_t log_next = 0;
    bool range_truncated = false;
    cap_lua_async_copy_log_range_locked(&s_jobs[slot],
                                        log_from,
                                        CAP_LUA_ASYNC_LOG_TAIL_DEFAULT_BYTES,
                                        recent_log,
                                        recent_log_size,
                                        &log_from,
                                        &log_next,
                                        &range_truncated);

    snprintf(output, output_size,
             "job_id=%s\nname=%s\nstatus=%s\nexclusive=%s\nruntime_s=%d\npath=%s\nargs=%s\nsummary=%s\n"
             "log_seq=%" PRIu64 "\nlog_size=%u\nlog_truncated=%s\nrecent_log=%s",
             s_jobs[slot].job_id,
             s_jobs[slot].name[0] ? s_jobs[slot].name : "(unnamed)",
             cap_lua_job_status_name(s_jobs[slot].status),
             s_jobs[slot].exclusive[0] ? s_jobs[slot].exclusive : "none",
             runtime_s,
             s_jobs[slot].path,
             (s_jobs[slot].args_json && s_jobs[slot].args_json[0]) ? s_jobs[slot].args_json : "(none)",
             (s_jobs[slot].summary && s_jobs[slot].summary[0]) ? s_jobs[slot].summary : "(empty)",
             s_jobs[slot].log_seq,
             (unsigned)s_jobs[slot].log_size,
             (s_jobs[slot].log_truncated || range_truncated) ? "true" : "false",
             recent_log[0] ? recent_log : "(empty)");
    xSemaphoreGive(s_job_lock);
    free(recent_log);
    return ESP_OK;
}

esp_err_t cap_lua_async_tail_job(const char *id_or_name,
                                 bool has_since_seq,
                                 uint64_t since_seq,
                                 size_t max_bytes,
                                 char *output,
                                 size_t output_size)
{
    char *log = NULL;
    size_t log_size;

    if (!id_or_name || !id_or_name[0] || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    if (max_bytes == 0) {
        max_bytes = CAP_LUA_ASYNC_LOG_TAIL_DEFAULT_BYTES;
    }
    if (output_size <= 512) {
        max_bytes = 0;
    } else if (max_bytes > output_size - 512) {
        max_bytes = output_size - 512;
    }
    if (max_bytes > CAP_LUA_ASYNC_LOG_MAX_BYTES) {
        max_bytes = CAP_LUA_ASYNC_LOG_MAX_BYTES;
    }
    log_size = max_bytes + 1;
    log = calloc(1, log_size);
    if (!log) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(log);
        return ESP_ERR_TIMEOUT;
    }

    int slot = cap_lua_find_slot_by_id_or_name_locked(id_or_name);
    if (slot < 0) {
        xSemaphoreGive(s_job_lock);
        free(log);
        snprintf(output, output_size, "Error: Lua async job not found: %s", id_or_name);
        return ESP_ERR_NOT_FOUND;
    }

    uint64_t from_seq = since_seq;
    if (!has_since_seq) {
        from_seq = s_jobs[slot].log_seq > max_bytes ? s_jobs[slot].log_seq - max_bytes : 0;
    }
    uint64_t actual_from = 0;
    uint64_t next_seq = 0;
    bool range_truncated = false;
    cap_lua_async_copy_log_range_locked(&s_jobs[slot],
                                        from_seq,
                                        max_bytes,
                                        log,
                                        log_size,
                                        &actual_from,
                                        &next_seq,
                                        &range_truncated);

    snprintf(output,
             output_size,
             "job_id=%s\nstatus=%s\nlog_from_seq=%" PRIu64 "\nlog_next_seq=%" PRIu64
             "\nlog_truncated=%s\nlog=%s",
             s_jobs[slot].job_id,
             cap_lua_job_status_name(s_jobs[slot].status),
             actual_from,
             next_seq,
             (s_jobs[slot].log_truncated || range_truncated) ? "true" : "false",
             log[0] ? log : "(empty)");
    xSemaphoreGive(s_job_lock);
    free(log);
    return ESP_OK;
}

esp_err_t cap_lua_async_stop_job(const char *id_or_name,
                                 uint32_t wait_ms,
                                 char *output,
                                 size_t output_size)
{
    if (!id_or_name || !id_or_name[0]) {
        if (output && output_size > 0) {
            snprintf(output, output_size, "Error: missing job_id or name");
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int slot = cap_lua_find_slot_by_id_or_name_locked(id_or_name);
    if (slot < 0) {
        xSemaphoreGive(s_job_lock);
        if (output && output_size > 0) {
            snprintf(output, output_size, "Error: Lua async job not found: %s", id_or_name);
        }
        return ESP_ERR_NOT_FOUND;
    }
    char job_id[CAP_LUA_JOB_ID_LEN];
    strlcpy(job_id, s_jobs[slot].job_id, sizeof(job_id));
    xSemaphoreGive(s_job_lock);

    if (wait_ms == 0) {
        wait_ms = CAP_LUA_STOP_WAIT_DEFAULT_MS;
    }
    bool was_running = false;
    esp_err_t err = cap_lua_stop_slot_and_wait(slot, job_id, wait_ms, &was_running);

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        const char *status_name = "gone";
        if (s_jobs[slot].used &&
                strncmp(s_jobs[slot].job_id, job_id, sizeof(job_id)) == 0) {
            status_name = cap_lua_job_status_name(s_jobs[slot].status);
        }
        if (err == ESP_OK && was_running) {
            snprintf(output, output_size, "OK: stopped job %s (status=%s)",
                     job_id, status_name);
        } else if (err == ESP_OK && !was_running) {
            snprintf(output, output_size, "OK: job %s already terminal (status=%s)",
                     job_id, status_name);
        } else if (err == ESP_ERR_TIMEOUT) {
            snprintf(output, output_size,
                     "WARN: stop requested for job %s but task did not exit within %u ms (status=%s)",
                     job_id, (unsigned)wait_ms, status_name);
        } else {
            snprintf(output, output_size, "Error stopping job %s: %s",
                     job_id, esp_err_to_name(err));
        }
        xSemaphoreGive(s_job_lock);
    }
    return err;
}

esp_err_t cap_lua_async_stop_all_jobs(const char *exclusive_filter,
                                      uint32_t wait_ms,
                                      char *output,
                                      size_t output_size)
{
    struct {
        int slot;
        char job_id[CAP_LUA_JOB_ID_LEN];
    } targets[CAP_LUA_ASYNC_MAX_JOBS];
    int target_count = 0;

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (int i = 0; i < CAP_LUA_ASYNC_MAX_JOBS; i++) {
        if (!s_jobs[i].used || cap_lua_status_is_terminal(s_jobs[i].status)) {
            continue;
        }
        if (exclusive_filter && exclusive_filter[0]) {
            if (strcmp(s_jobs[i].exclusive, exclusive_filter) != 0) {
                continue;
            }
        }
        targets[target_count].slot = i;
        strlcpy(targets[target_count].job_id, s_jobs[i].job_id,
                sizeof(targets[target_count].job_id));
        target_count++;
    }
    xSemaphoreGive(s_job_lock);

    if (wait_ms == 0) {
        wait_ms = CAP_LUA_STOP_WAIT_DEFAULT_MS;
    }

    int stopped = 0;
    int timed_out = 0;
    for (int t = 0; t < target_count; t++) {
        if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        bool same_job = (s_jobs[targets[t].slot].used &&
                         strncmp(s_jobs[targets[t].slot].job_id,
                                 targets[t].job_id,
                                 sizeof(targets[t].job_id)) == 0);
        xSemaphoreGive(s_job_lock);
        if (!same_job) {
            continue;
        }
        bool was_running = false;
        esp_err_t err = cap_lua_stop_slot_and_wait(targets[t].slot,
                                                   targets[t].job_id,
                                                   wait_ms,
                                                   &was_running);
        if (err == ESP_OK) {
            stopped++;
        } else if (err == ESP_ERR_TIMEOUT) {
            timed_out++;
        }
    }

    if (output && output_size > 0) {
        const char *filter_label = (exclusive_filter && exclusive_filter[0]) ? exclusive_filter : "all";
        if (target_count == 0) {
            snprintf(output, output_size, "No matching jobs (filter=%s)", filter_label);
        } else {
            snprintf(output, output_size,
                     "Stopped %d job(s), %d still unwinding (filter=%s)",
                     stopped, timed_out, filter_label);
        }
    }
    return ESP_OK;
}

esp_err_t cap_lua_async_wait_settle(const char *job_id,
                                    uint32_t timeout_ms,
                                    cap_lua_job_status_t *out_status,
                                    char *summary_out,
                                    size_t summary_out_size)
{
    int slot = -1;

    if (!job_id || !job_id[0] || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    if (summary_out && summary_out_size > 0) {
        summary_out[0] = '\0';
    }
    if (!s_job_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    slot = cap_lua_find_slot_by_id_or_name_locked(job_id);
    if (slot < 0) {
        xSemaphoreGive(s_job_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *out_status = s_jobs[slot].status;
    bool already_terminal = cap_lua_status_is_terminal(*out_status);
    xSemaphoreGive(s_job_lock);

    if (!already_terminal && timeout_ms > 0) {
        cap_lua_wait_for_terminal(slot, timeout_ms);
    }

    return cap_lua_async_get_status(job_id, out_status, summary_out, summary_out_size);
}

esp_err_t cap_lua_async_get_status(const char *job_id,
                                   cap_lua_job_status_t *out_status,
                                   char *summary_out,
                                   size_t summary_out_size)
{
    if (!job_id || !job_id[0] || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    if (summary_out && summary_out_size > 0) {
        summary_out[0] = '\0';
    }
    if (!s_job_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int slot = cap_lua_find_slot_by_id_or_name_locked(job_id);
    if (slot < 0) {
        xSemaphoreGive(s_job_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *out_status = s_jobs[slot].status;
    if (summary_out && summary_out_size > 0 && s_jobs[slot].summary && s_jobs[slot].summary[0]) {
        strlcpy(summary_out, s_jobs[slot].summary, summary_out_size);
    }
    xSemaphoreGive(s_job_lock);
    return ESP_OK;
}

size_t cap_lua_async_collect_active_snapshots(cap_lua_async_job_snapshot_t *out,
                                              size_t max)
{
    size_t count = 0;

    if (!out || max == 0 || !s_job_lock) {
        return 0;
    }
    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return 0;
    }
    for (int i = 0; i < CAP_LUA_ASYNC_MAX_JOBS && count < max; i++) {
        if (!s_jobs[i].used || cap_lua_status_is_terminal(s_jobs[i].status)) {
            continue;
        }
        memset(&out[count], 0, sizeof(out[count]));
        strlcpy(out[count].job_id, s_jobs[i].job_id, sizeof(out[count].job_id));
        strlcpy(out[count].name, s_jobs[i].name, sizeof(out[count].name));
        strlcpy(out[count].exclusive, s_jobs[i].exclusive, sizeof(out[count].exclusive));
        strlcpy(out[count].path, s_jobs[i].path, sizeof(out[count].path));
        out[count].status = s_jobs[i].status;
        out[count].created_at = s_jobs[i].created_at;
        out[count].started_at = s_jobs[i].started_at;
        out[count].finished_at = s_jobs[i].finished_at;
        count++;
    }
    xSemaphoreGive(s_job_lock);
    return count;
}

size_t cap_lua_async_active_count(void)
{
    size_t count = 0;

    if (!s_job_lock) {
        return 0;
    }
    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        /* Fail closed: callers (e.g. deactivate guard) MUST treat unknown as
         * non-zero, otherwise contention on the job lock would silently allow
         * the dangerous operation through. */
        return SIZE_MAX;
    }
    for (int i = 0; i < CAP_LUA_ASYNC_MAX_JOBS; i++) {
        if (s_jobs[i].used && !cap_lua_status_is_terminal(s_jobs[i].status)) {
            count++;
        }
    }
    xSemaphoreGive(s_job_lock);
    return count;
}
