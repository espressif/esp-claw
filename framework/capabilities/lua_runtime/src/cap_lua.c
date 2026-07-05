/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_lua.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_core.h"
#include "esp_check.h"
#include "esp_log.h"

#include "cap_lua_internal.h"

static const char *TAG = "cap_lua";

typedef struct cap_lua_exit_cleanup_node {
    cap_lua_exit_cleanup_fn_t cleanup_fn;
    struct cap_lua_exit_cleanup_node *next;
} cap_lua_exit_cleanup_node_t;

typedef struct cap_lua_package_path_dir_node {
    char dir[CAP_LUA_JOB_PATH_MAX];
    struct cap_lua_package_path_dir_node *next;
} cap_lua_package_path_dir_node_t;

static EXT_RAM_BSS_ATTR cap_lua_module_t s_modules[CAP_LUA_MAX_MODULES];
static cap_lua_package_path_dir_node_t *s_package_path_dirs;
static size_t s_package_path_dir_count;
static size_t s_module_count;
static cap_lua_exit_cleanup_node_t *s_exit_cleanups;
static size_t s_exit_cleanup_count;
static bool s_builtin_modules_registered;
static bool s_module_registration_locked;

static bool cap_lua_abs_dir_is_valid(const char *dir);

static esp_err_t cap_lua_build_simple_request(const char *string_key,
                                              const char *string_value,
                                              const char *string_key2,
                                              const char *string_value2,
                                              bool has_bool,
                                              const char *bool_key,
                                              bool bool_value,
                                              bool has_number,
                                              const char *number_key,
                                              uint32_t number_value,
                                              char **json_out)
{
    cJSON *root = NULL;

    if (!json_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *json_out = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    if (string_key && string_value && !cJSON_AddStringToObject(root, string_key, string_value)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (string_key2 && string_value2 &&
            !cJSON_AddStringToObject(root, string_key2, string_value2)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (has_bool && bool_key && !cJSON_AddBoolToObject(root, bool_key, bool_value)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (has_number && number_key &&
            !cJSON_AddNumberToObject(root, number_key, (double)number_value)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    *json_out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *json_out ? ESP_OK : ESP_ERR_NO_MEM;
}

size_t cap_lua_get_package_path_dir_count(void)
{
    return s_package_path_dir_count;
}

const char *cap_lua_get_package_path_dir(size_t index)
{
    cap_lua_package_path_dir_node_t *node = s_package_path_dirs;
    size_t i = 0;

    while (node) {
        if (i == index) {
            return node->dir;
        }
        node = node->next;
        i++;
    }

    return NULL;
}

esp_err_t cap_lua_add_package_path_dir(const char *dir)
{
    cap_lua_package_path_dir_node_t *node = NULL;
    cap_lua_package_path_dir_node_t **tail = &s_package_path_dirs;

    if (!cap_lua_abs_dir_is_valid(dir)) {
        ESP_LOGE(TAG, "add_package_path_dir: bad dir=%s", dir ? dir : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(dir) >= CAP_LUA_JOB_PATH_MAX) {
        ESP_LOGE(TAG, "add_package_path_dir: dir too long");
        return ESP_ERR_INVALID_SIZE;
    }

    while (*tail) {
        if (strcmp((*tail)->dir, dir) == 0) {
            return ESP_OK;
        }
        tail = &(*tail)->next;
    }

    node = calloc(1, sizeof(*node));
    if (!node) {
        ESP_LOGE(TAG, "add_package_path_dir: no memory for dir=%s", dir);
        return ESP_ERR_NO_MEM;
    }

    /* Keep a private copy so callers can pass stack-backed path buffers safely. */
    strlcpy(node->dir, dir, sizeof(node->dir));
    *tail = node;
    s_package_path_dir_count++;
    return ESP_OK;
}

static bool cap_lua_has_lua_suffix(const char *path)
{
    size_t path_len;

    if (!path) {
        return false;
    }

    path_len = strlen(path);
    return path_len > 4 && strcmp(path + path_len - 4, ".lua") == 0;
}

static bool cap_lua_abs_dir_is_valid(const char *dir)
{
    return dir && dir[0] == '/' && strstr(dir, "..") == NULL;
}

bool cap_lua_run_path_is_valid(const char *path)
{
    /* Run targets must be absolute .lua paths with no ".." segments. The script root is no
     * longer enforced here so callers can launch scripts from any mount point (e.g. RAMFS). */
    return path && path[0] == '/' &&
           strstr(path, "..") == NULL &&
           cap_lua_has_lua_suffix(path);
}

esp_err_t cap_lua_resolve_run_path(const char *path, char *resolved, size_t resolved_size)
{
    if (!path || !path[0] || !resolved || resolved_size == 0) {
        ESP_LOGE(TAG, "resolve_run_path: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }
    /* Run tools only accept absolute .lua paths; the script's mount point is unrestricted so a
     * caller can run a script from /fatfs/scripts, /fatfs/skills, /fatfs/temp (RAMFS), etc. */
    if (!cap_lua_run_path_is_valid(path)) {
        ESP_LOGE(TAG, "resolve_run_path: path must be an absolute .lua path: %s", path);
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(resolved, path, resolved_size) >= resolved_size) {
        ESP_LOGE(TAG, "resolve_run_path: path too long");
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t cap_lua_build_args_json(cJSON *root, char **args_json_out)
{
    cJSON *args = NULL;
    cJSON *payload = NULL;

    if (!args_json_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *args_json_out = NULL;

    args = cJSON_GetObjectItem(root, "args");
    if (cJSON_IsObject(args)) {
        payload = cJSON_Duplicate(args, 1);
        if (!payload) {
            return ESP_ERR_NO_MEM;
        }
        *args_json_out = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
        if (!*args_json_out) {
            return ESP_ERR_NO_MEM;
        }
    } else if (args) {
        ESP_LOGE(TAG, "build_args_json: args must be object");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t cap_lua_group_init(void)
{
    ESP_RETURN_ON_ERROR(cap_lua_register_builtin_modules(),
                        TAG,
                        "Failed to register builtin Lua modules");
    s_module_registration_locked = true;
    ESP_RETURN_ON_ERROR(cap_lua_runtime_init(), TAG, "Failed to init runtime");
    ESP_RETURN_ON_ERROR(cap_lua_async_init(), TAG, "Failed to init async runner");
    return ESP_OK;
}

size_t cap_lua_get_active_async_job_count(void)
{
    return cap_lua_async_active_count();
}

static esp_err_t cap_lua_group_start(void)
{
    return cap_lua_async_start();
}

static esp_err_t cap_lua_run_script_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    char resolved_path[192];
    cJSON *timeout_item = NULL;
    char *args_json = NULL;
    uint32_t timeout_ms = 0;
    cap_lua_async_job_t job = {0};
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        ESP_LOGE(TAG, "run_script: invalid json");
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (cap_lua_resolve_run_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "run_script: bad path=%s", path ? path : "(null)");
        snprintf(output, output_size, "Error: path must be an absolute .lua path");
        return ESP_ERR_INVALID_ARG;
    }

    timeout_item = cJSON_GetObjectItem(root, "timeout_ms");
    if (timeout_item && (!cJSON_IsNumber(timeout_item) || timeout_item->valueint <= 0)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: timeout_ms must be a positive integer");
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_IsNumber(timeout_item)) {
        timeout_ms = (uint32_t)timeout_item->valueint;
    }

    err = cap_lua_build_args_json(root, &args_json);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        free(args_json);
        if (err == ESP_ERR_INVALID_ARG) {
            snprintf(output, output_size, "Error: args must be a JSON object");
        } else {
            snprintf(output, output_size, "Error: failed to prepare Lua args");
        }
        return err;
    }

    if (timeout_ms == 0) {
        timeout_ms = CAP_LUA_SYNC_DEFAULT_TIMEOUT_MS;
    }
    strlcpy(job.path, resolved_path, sizeof(job.path));
    job.args_json = args_json;
    job.timeout_ms = timeout_ms;
    job.log_bytes = CAP_LUA_ASYNC_LOG_DEFAULT_BYTES;
    job.sync_waiter = true;
    job.created_at = time(NULL);
    err = cap_lua_async_run_and_wait(&job, timeout_ms, output, output_size);
    free(args_json);
    return err;
}

static const char *cap_lua_basename_from_path(const char *path)
{
    const char *slash = NULL;

    if (!path) {
        return NULL;
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static esp_err_t cap_lua_run_script_async_execute(const char *input_json,
                                                  const claw_cap_call_context_t *ctx,
                                                  char *output,
                                                  size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    const char *name = NULL;
    const char *exclusive = NULL;
    char resolved_path[192];
    cJSON *timeout_item = NULL;
    cJSON *log_bytes_item = NULL;
    cJSON *replace_item = NULL;
    char *args_json = NULL;
    char request_path[192] = {0};
    uint32_t timeout_ms = CAP_LUA_ASYNC_DEFAULT_TIMEOUT_MS;
    size_t log_bytes = CAP_LUA_ASYNC_LOG_DEFAULT_BYTES;
    cap_lua_async_job_t job = {0};
    char job_id[CAP_LUA_JOB_ID_LEN] = {0};
    char err_buf[256] = {0};
    bool replace = false;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        ESP_LOGE(TAG, "run_async: invalid json");
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (cap_lua_resolve_run_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "run_async: bad path=%s", path ? path : "(null)");
        snprintf(output, output_size, "Error: path must be an absolute .lua path");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(request_path, path ? path : resolved_path, sizeof(request_path));

    {
        struct stat script_stat;
        if (stat(resolved_path, &script_stat) != 0) {
            cJSON_Delete(root);
            ESP_LOGE(TAG, "run_async: missing script path=%s errno=%d", request_path, errno);
            snprintf(output, output_size, "Error: script not found: %s (errno=%d)", request_path, errno);
            return ESP_ERR_NOT_FOUND;
        }
    }

    timeout_item = cJSON_GetObjectItem(root, "timeout_ms");
    if (timeout_item && (!cJSON_IsNumber(timeout_item) || timeout_item->valueint < 0)) {
        cJSON_Delete(root);
        snprintf(output, output_size,
                 "Error: timeout_ms must be a non-negative integer (0 = until cancelled)");
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_IsNumber(timeout_item)) {
        timeout_ms = (uint32_t)timeout_item->valueint;
    }

    log_bytes_item = cJSON_GetObjectItem(root, "log_bytes");
    if (log_bytes_item) {
        if (!cJSON_IsNumber(log_bytes_item) ||
                log_bytes_item->valueint < CAP_LUA_ASYNC_LOG_MIN_BYTES ||
                log_bytes_item->valueint > CAP_LUA_ASYNC_LOG_MAX_BYTES) {
            cJSON_Delete(root);
            snprintf(output,
                     output_size,
                     "Error: log_bytes must be an integer between %u and %u",
                     (unsigned)CAP_LUA_ASYNC_LOG_MIN_BYTES,
                     (unsigned)CAP_LUA_ASYNC_LOG_MAX_BYTES);
            return ESP_ERR_INVALID_ARG;
        }
        log_bytes = (size_t)log_bytes_item->valueint;
    }

    name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    exclusive = cJSON_GetStringValue(cJSON_GetObjectItem(root, "exclusive"));
    replace_item = cJSON_GetObjectItem(root, "replace");
    if (cJSON_IsBool(replace_item)) {
        replace = cJSON_IsTrue(replace_item);
    }

    strlcpy(job.path, resolved_path, sizeof(job.path));
    if (name && name[0]) {
        strlcpy(job.name, name, sizeof(job.name));
    } else {
        const char *base = cap_lua_basename_from_path(resolved_path);
        if (base) {
            strlcpy(job.name, base, sizeof(job.name));
        }
    }
    if (exclusive && exclusive[0]) {
        strlcpy(job.exclusive, exclusive, sizeof(job.exclusive));
    }

    err = cap_lua_build_args_json(root, &args_json);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        free(args_json);
        if (err == ESP_ERR_INVALID_ARG) {
            snprintf(output, output_size, "Error: args must be a JSON object");
        } else {
            snprintf(output, output_size, "Error: failed to prepare Lua args");
        }
        return err;
    }

    job.args_json = args_json;
    job.timeout_ms = timeout_ms;
    job.log_bytes = log_bytes;
    job.replace = replace;
    job.created_at = time(NULL);
    err = cap_lua_async_submit(&job, job_id, sizeof(job_id), err_buf, sizeof(err_buf));
    free(args_json);
    if (err != ESP_OK) {
        if (err_buf[0]) {
            snprintf(output, output_size, "Error: %s", err_buf);
        } else if (err == ESP_ERR_INVALID_STATE) {
            snprintf(output, output_size, "Error: Lua async runner is not ready");
        } else {
            snprintf(output, output_size, "Error: failed to queue async Lua job (%s)",
                     esp_err_to_name(err));
        }
        return err;
    }

    cap_lua_job_status_t settle_status = CAP_LUA_JOB_RUNNING;
    char settle_summary[128] = {0};
    cap_lua_async_wait_settle(job_id, 150, &settle_status, settle_summary, sizeof(settle_summary));

    const char *status_label = cap_lua_job_status_name(settle_status);

    if (settle_status == CAP_LUA_JOB_FAILED || settle_status == CAP_LUA_JOB_TIMEOUT ||
        settle_status == CAP_LUA_JOB_STOPPED) {
        snprintf(output, output_size,
                 "Lua job %s (name=%s) ended early with status=%s. summary: %s",
                 job_id,
                 job.name[0] ? job.name : "(unnamed)",
                 status_label,
                 settle_summary[0] ? settle_summary : "(none)");
        return ESP_OK;
    }

    snprintf(output, output_size,
             "Started Lua job %s (name=%s, exclusive=%s, timeout_ms=%u%s, log_bytes=%u, status=%s) "
             "for %s. Use lua_get_async_job or lua_tail_async_job with job_id=%s to read logs/results.",
             job_id,
             job.name[0] ? job.name : "(unnamed)",
             job.exclusive[0] ? job.exclusive : "none",
             (unsigned)timeout_ms,
             timeout_ms == 0 ? " [until cancelled]" : "",
             (unsigned)log_bytes,
             status_label,
             request_path,
             job_id);
    return ESP_OK;
}

static esp_err_t cap_lua_stop_async_job_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    cJSON *root = NULL;
    const char *target = NULL;
    cJSON *wait_item = NULL;
    uint32_t wait_ms = 0;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }
    target = cJSON_GetStringValue(cJSON_GetObjectItem(root, "job_id"));
    if (!target || !target[0]) {
        target = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    }
    if (!target || !target[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: provide either 'job_id' or 'name'");
        return ESP_ERR_INVALID_ARG;
    }
    wait_item = cJSON_GetObjectItem(root, "wait_ms");
    if (cJSON_IsNumber(wait_item) && wait_item->valueint > 0) {
        wait_ms = (uint32_t)wait_item->valueint;
    }

    err = cap_lua_async_stop_job(target, wait_ms, output, output_size);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_lua_stop_all_async_jobs_execute(const char *input_json,
                                                     const claw_cap_call_context_t *ctx,
                                                     char *output,
                                                     size_t output_size)
{
    cJSON *root = NULL;
    const char *exclusive = NULL;
    cJSON *wait_item = NULL;
    uint32_t wait_ms = 0;
    esp_err_t err;

    (void)ctx;

    if (input_json && input_json[0]) {
        root = cJSON_Parse(input_json);
        if (!root) {
            snprintf(output, output_size, "Error: invalid JSON");
            return ESP_ERR_INVALID_ARG;
        }
        exclusive = cJSON_GetStringValue(cJSON_GetObjectItem(root, "exclusive"));
        wait_item = cJSON_GetObjectItem(root, "wait_ms");
        if (cJSON_IsNumber(wait_item) && wait_item->valueint > 0) {
            wait_ms = (uint32_t)wait_item->valueint;
        }
    }

    err = cap_lua_async_stop_all_jobs(exclusive, wait_ms, output, output_size);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_lua_list_async_jobs_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size)
{
    cJSON *root = NULL;
    const char *status = NULL;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (root) {
        status = cJSON_GetStringValue(cJSON_GetObjectItem(root, "status"));
        if (status &&
                strcmp(status, "all") != 0 &&
                strcmp(status, "queued") != 0 &&
                strcmp(status, "running") != 0 &&
                strcmp(status, "done") != 0 &&
                strcmp(status, "failed") != 0 &&
                strcmp(status, "timeout") != 0 &&
                strcmp(status, "stopped") != 0) {
            cJSON_Delete(root);
            snprintf(output,
                     output_size,
                     "Error: status must be one of all, queued, running, done, failed, timeout, stopped");
            return ESP_ERR_INVALID_ARG;
        }
    }

    err = cap_lua_async_list_jobs(status, output, output_size);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_lua_get_async_job_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    cJSON *root = NULL;
    const char *job_id = NULL;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    job_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "job_id"));
    if (!job_id || !job_id[0]) {
        job_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    }
    if (!job_id || !job_id[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: provide either 'job_id' or 'name'");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_lua_async_get_job(job_id, output, output_size);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_lua_tail_async_job_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    cJSON *root = NULL;
    const char *job_id = NULL;
    cJSON *since_item = NULL;
    cJSON *max_item = NULL;
    bool has_since_seq = false;
    uint64_t since_seq = 0;
    size_t max_bytes = CAP_LUA_ASYNC_LOG_TAIL_DEFAULT_BYTES;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    job_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "job_id"));
    if (!job_id || !job_id[0]) {
        job_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    }
    if (!job_id || !job_id[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: provide either 'job_id' or 'name'");
        return ESP_ERR_INVALID_ARG;
    }

    since_item = cJSON_GetObjectItem(root, "since_seq");
    if (since_item) {
        if (!cJSON_IsNumber(since_item) || since_item->valuedouble < 0) {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: since_seq must be a non-negative integer");
            return ESP_ERR_INVALID_ARG;
        }
        has_since_seq = true;
        since_seq = (uint64_t)since_item->valuedouble;
    }

    max_item = cJSON_GetObjectItem(root, "max_bytes");
    if (max_item) {
        if (!cJSON_IsNumber(max_item) || max_item->valueint <= 0) {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: max_bytes must be a positive integer");
            return ESP_ERR_INVALID_ARG;
        }
        max_bytes = (size_t)max_item->valueint;
    }

    err = cap_lua_async_tail_job(job_id,
                                 has_since_seq,
                                 since_seq,
                                 max_bytes,
                                 output,
                                 output_size);
    cJSON_Delete(root);
    return err;
}

static const claw_cap_descriptor_t s_lua_descriptors[] = {
    {
        .id = "lua_run_script",
        .name = "lua_run_script",
        .family = "automation",
        .description = "Run a Lua script synchronously with optional args and timeout.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"args\":{\"type\":\"object\","
        "\"description\":\"Lua script arguments object keyed by parameter name.\","
        "\"additionalProperties\":true},"
        "\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"path\"]}",
        .execute = cap_lua_run_script_execute,
    },
    {
        .id = "lua_run_script_async",
        .name = "lua_run_script_async",
        .family = "automation",
        .description =
        "Run Lua async; returns job id. timeout_ms=0 runs until cancelled. "
        "Use name/exclusive for conflicts; replace=true takes over. Read running, "
        "final, or failed-job logs with lua_get_async_job or lua_tail_async_job "
        "using returned job id.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"args\":{\"type\":\"object\","
        "\"description\":\"Lua script arguments object keyed by parameter name.\","
        "\"additionalProperties\":true},"
        "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":0},\"log_bytes\":{\"type\":\"integer\","
        "\"minimum\":1024,\"maximum\":16384},\"name\":{\"type\":\"string\"},"
        "\"exclusive\":{\"type\":\"string\"},\"replace\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}",
        .execute = cap_lua_run_script_async_execute,
    },
    {
        .id = "lua_list_async_jobs",
        .name = "lua_list_async_jobs",
        .family = "automation",
        .description = "List Lua async jobs by optional status filter.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\",\"enum\":[\"all\",\"queued\",\"running\",\"done\",\"failed\",\"timeout\",\"stopped\"]}}}",
        .execute = cap_lua_list_async_jobs_execute,
    },
    {
        .id = "lua_get_async_job",
        .name = "lua_get_async_job",
        .family = "automation",
        .description = "Get status, summary, and recent logs for a Lua async job by job_id or name.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}}}",
        .execute = cap_lua_get_async_job_execute,
    },
    {
        .id = "lua_tail_async_job",
        .name = "lua_tail_async_job",
        .family = "automation",
        .description =
        "Read incremental logs for a Lua async job by job_id or name. Pass since_seq from the "
        "previous log_next_seq to continue reading only new log text.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"},"
        "\"name\":{\"type\":\"string\"},\"since_seq\":{\"type\":\"integer\",\"minimum\":0},"
        "\"max_bytes\":{\"type\":\"integer\",\"minimum\":1}}}",
        .execute = cap_lua_tail_async_job_execute,
    },
    {
        .id = "lua_stop_async_job",
        .name = "lua_stop_async_job",
        .family = "automation",
        .description =
        "Stop a running Lua async job by job_id or name. MUST be called whenever the user asks "
        "to stop, cancel, quit or close an async script; replying without calling this leaves "
        "the job running. Cooperative; default wait 2000 ms.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"wait_ms\":{\"type\":\"integer\",\"minimum\":1}}}",
        .execute = cap_lua_stop_async_job_execute,
    },
    {
        .id = "lua_stop_all_async_jobs",
        .name = "lua_stop_all_async_jobs",
        .family = "automation",
        .description =
        "Stop all running Lua async jobs, optionally filtered by exclusive group "
        "(e.g. exclusive='display'). MUST be called when the user asks to clear the screen, "
        "stop everything or cancel all background scripts.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"exclusive\":{\"type\":\"string\"},\"wait_ms\":{\"type\":\"integer\",\"minimum\":1}}}",
        .execute = cap_lua_stop_all_async_jobs_execute,
    },
};

static const claw_cap_group_t s_lua_group = {
    .group_id = "cap_lua",
    .descriptors = s_lua_descriptors,
    .descriptor_count = sizeof(s_lua_descriptors) / sizeof(s_lua_descriptors[0]),
    .group_init = cap_lua_group_init,
    .group_start = cap_lua_group_start,
};

esp_err_t cap_lua_register_group(void)
{
    if (claw_cap_group_exists(s_lua_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_lua_group);
}

esp_err_t cap_lua_run_script(const char *path,
                             const char *args_json,
                             uint32_t timeout_ms,
                             char *output,
                             size_t output_size)
{
    cJSON *root = NULL;
    cJSON *args = NULL;
    char *input_json = NULL;
    esp_err_t err = ESP_OK;

    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(root, "path", path)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (args_json && args_json[0]) {
        args = cJSON_Parse(args_json);
        if (!args || !cJSON_IsObject(args)) {
            cJSON_Delete(args);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddItemToObject(root, "args", args);
    }
    if (timeout_ms > 0 && !cJSON_AddNumberToObject(root, "timeout_ms", (double)timeout_ms)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    input_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!input_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_lua_run_script_execute(input_json, NULL, output, output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_run_script_async(const char *path,
                                   const char *args_json,
                                   uint32_t timeout_ms,
                                   const char *name,
                                   const char *exclusive,
                                   bool replace,
                                   char *output,
                                   size_t output_size)
{
    cJSON *root = NULL;
    cJSON *args = NULL;
    char *input_json = NULL;
    esp_err_t err = ESP_OK;

    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(root, "path", path)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (args_json && args_json[0]) {
        args = cJSON_Parse(args_json);
        if (!args || !cJSON_IsObject(args)) {
            cJSON_Delete(args);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddItemToObject(root, "args", args);
    }
    if (!cJSON_AddNumberToObject(root, "timeout_ms", (double)timeout_ms)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (name && name[0] && !cJSON_AddStringToObject(root, "name", name)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (exclusive && exclusive[0] && !cJSON_AddStringToObject(root, "exclusive", exclusive)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (replace && !cJSON_AddBoolToObject(root, "replace", true)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    input_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!input_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_lua_run_script_async_execute(input_json, NULL, output, output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_stop_job(const char *id_or_name,
                           uint32_t wait_ms,
                           char *output,
                           size_t output_size)
{
    return cap_lua_async_stop_job(id_or_name, wait_ms, output, output_size);
}

esp_err_t cap_lua_stop_all_jobs(const char *exclusive_filter,
                                uint32_t wait_ms,
                                char *output,
                                size_t output_size)
{
    return cap_lua_async_stop_all_jobs(exclusive_filter, wait_ms, output, output_size);
}

esp_err_t cap_lua_list_jobs(const char *status, char *output, size_t output_size)
{
    char *input_json = NULL;
    esp_err_t err;

    err = cap_lua_build_simple_request("status",
                                       status,
                                       NULL,
                                       NULL,
                                       false,
                                       NULL,
                                       false,
                                       false,
                                       NULL,
                                       0,
                                       &input_json);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_lua_list_async_jobs_execute(input_json ? input_json : "{}",
                                          NULL,
                                          output,
                                          output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_get_job(const char *job_id, char *output, size_t output_size)
{
    char *input_json = NULL;
    esp_err_t err;

    err = cap_lua_build_simple_request("job_id",
                                       job_id,
                                       NULL,
                                       NULL,
                                       false,
                                       NULL,
                                       false,
                                       false,
                                       NULL,
                                       0,
                                       &input_json);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_lua_get_async_job_execute(input_json ? input_json : "{}",
                                        NULL,
                                        output,
                                        output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_register_module(const char *name, lua_CFunction open_fn)
{
    size_t i;

    if (!name || !name[0] || !open_fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_module_registration_locked) {
        return ESP_ERR_INVALID_STATE;
    }

    for (i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].name, name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (s_module_count >= CAP_LUA_MAX_MODULES) {
        ESP_LOGE(TAG,
                 "Lua module registry full: tried to add '%s' with %u/%u slots used",
                 name,
                 (unsigned)s_module_count,
                 (unsigned)CAP_LUA_MAX_MODULES);
        return ESP_ERR_NO_MEM;
    }

    s_modules[s_module_count].name = name;
    s_modules[s_module_count].open_fn = open_fn;
    s_module_count++;
    return ESP_OK;
}

esp_err_t cap_lua_register_modules(const cap_lua_module_t *modules, size_t count)
{
    size_t i;
    esp_err_t err;

    if (!modules || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (i = 0; i < count; i++) {
        err = cap_lua_register_module(modules[i].name, modules[i].open_fn);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t cap_lua_register_exit_cleanup(cap_lua_exit_cleanup_fn_t cleanup_fn)
{
    cap_lua_exit_cleanup_node_t *node = NULL;
    cap_lua_exit_cleanup_node_t *it = NULL;

    if (!cleanup_fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_module_registration_locked) {
        return ESP_ERR_INVALID_STATE;
    }

    for (it = s_exit_cleanups; it != NULL; it = it->next) {
        if (it->cleanup_fn == cleanup_fn) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    node = calloc(1, sizeof(*node));
    if (!node) {
        return ESP_ERR_NO_MEM;
    }

    node->cleanup_fn = cleanup_fn;
    node->next = s_exit_cleanups;
    s_exit_cleanups = node;
    s_exit_cleanup_count++;
    return ESP_OK;
}

esp_err_t cap_lua_register_builtin_modules(void)
{
    s_builtin_modules_registered = true;
    return ESP_OK;
}

size_t cap_lua_get_module_count(void)
{
    return s_module_count;
}

const cap_lua_module_t *cap_lua_get_module(size_t index)
{
    if (index >= s_module_count) {
        return NULL;
    }

    return &s_modules[index];
}

size_t cap_lua_get_exit_cleanup_count(void)
{
    return s_exit_cleanup_count;
}

cap_lua_exit_cleanup_fn_t cap_lua_get_exit_cleanup(size_t index)
{
    size_t i = 0;
    cap_lua_exit_cleanup_node_t *it = s_exit_cleanups;

    while (it != NULL) {
        if (i == index) {
            return it->cleanup_fn;
        }
        i++;
        it = it->next;
    }

    return NULL;
}
