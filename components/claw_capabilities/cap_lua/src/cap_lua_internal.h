/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "cap_lua.h"
#include "esp_err.h"

#define CAP_LUA_MAX_SCRIPT_SIZE         (16 * 1024)
#define CAP_LUA_OUTPUT_SIZE             (4 * 1024)
#define CAP_LUA_SYNC_DEFAULT_TIMEOUT_MS 60000
#define CAP_LUA_ASYNC_DEFAULT_TIMEOUT_MS 0
#define CAP_LUA_ASYNC_LOG_DEFAULT_BYTES (4 * 1024)
#define CAP_LUA_ASYNC_LOG_MIN_BYTES     (1 * 1024)
#define CAP_LUA_ASYNC_LOG_MAX_BYTES     (16 * 1024)
#define CAP_LUA_ASYNC_LOG_TAIL_DEFAULT_BYTES 2048
#define CAP_LUA_ASYNC_MAX_JOBS          16
#define CAP_LUA_ASYNC_MAX_CONCURRENT    4
#define CAP_LUA_MAX_MODULES             32

#define CAP_LUA_JOB_NAME_MAX            32
#define CAP_LUA_JOB_EXCLUSIVE_MAX       16
#define CAP_LUA_JOB_PATH_MAX            192
#define CAP_LUA_JOB_ID_LEN              9
#define CAP_LUA_STOP_WAIT_DEFAULT_MS    2000

typedef void (*cap_lua_runtime_log_fn_t)(void *user_ctx,
                                         const char *text,
                                         size_t len);

typedef struct {
    char path[CAP_LUA_JOB_PATH_MAX];
    char name[CAP_LUA_JOB_NAME_MAX];
    char exclusive[CAP_LUA_JOB_EXCLUSIVE_MAX];
    char *args_json;
    uint32_t timeout_ms;
    size_t log_bytes;
    bool replace;
    bool sync_waiter;
    time_t created_at;
} cap_lua_async_job_t;

typedef enum {
    CAP_LUA_JOB_QUEUED = 0,
    CAP_LUA_JOB_RUNNING,
    CAP_LUA_JOB_DONE,
    CAP_LUA_JOB_FAILED,
    CAP_LUA_JOB_TIMEOUT,
    CAP_LUA_JOB_STOPPED,
} cap_lua_job_status_t;

typedef struct {
    char job_id[CAP_LUA_JOB_ID_LEN];
    char name[CAP_LUA_JOB_NAME_MAX];
    char exclusive[CAP_LUA_JOB_EXCLUSIVE_MAX];
    char path[CAP_LUA_JOB_PATH_MAX];
    cap_lua_job_status_t status;
    time_t created_at;
    time_t started_at;
    time_t finished_at;
} cap_lua_async_job_snapshot_t;

const char *cap_lua_get_base_dir(void);
size_t cap_lua_get_package_path_dir_count(void);
const char *cap_lua_get_package_path_dir(size_t index);
bool cap_lua_path_is_valid(const char *path);
bool cap_lua_run_path_is_valid(const char *path);
esp_err_t cap_lua_resolve_path(const char *path, char *resolved, size_t resolved_size);
esp_err_t cap_lua_resolve_run_path(const char *path, char *resolved, size_t resolved_size);
esp_err_t cap_lua_ensure_base_dir(void);

esp_err_t cap_lua_runtime_init(void);

esp_err_t cap_lua_runtime_execute_file(const char *path,
                                       const char *args_json,
                                       uint32_t timeout_ms,
                                       volatile bool *stop_requested,
                                       cap_lua_runtime_log_fn_t log_fn,
                                       void *log_ctx,
                                       char *output,
                                       size_t output_size);
esp_err_t cap_lua_register_builtin_modules(void);
size_t cap_lua_get_module_count(void);
const cap_lua_module_t *cap_lua_get_module(size_t index);
size_t cap_lua_get_exit_cleanup_count(void);
cap_lua_exit_cleanup_fn_t cap_lua_get_exit_cleanup(size_t index);

esp_err_t cap_lua_async_init(void);
esp_err_t cap_lua_async_start(void);
esp_err_t cap_lua_async_submit(const cap_lua_async_job_t *job,
                               char *job_id_out,
                               size_t job_id_out_size,
                               char *err_out,
                               size_t err_out_size);
esp_err_t cap_lua_async_run_and_wait(const cap_lua_async_job_t *job,
                                     uint32_t wait_ms,
                                     char *output,
                                     size_t output_size);
esp_err_t cap_lua_async_list_jobs(const char *status_filter,
                                  char *output,
                                  size_t output_size);
esp_err_t cap_lua_async_get_job(const char *id_or_name,
                                char *output,
                                size_t output_size);
esp_err_t cap_lua_async_tail_job(const char *id_or_name,
                                 bool has_since_seq,
                                 uint64_t since_seq,
                                 size_t max_bytes,
                                 char *output,
                                 size_t output_size);
esp_err_t cap_lua_async_stop_job(const char *id_or_name,
                                 uint32_t wait_ms,
                                 char *output,
                                 size_t output_size);
esp_err_t cap_lua_async_stop_all_jobs(const char *exclusive_filter,
                                      uint32_t wait_ms,
                                      char *output,
                                      size_t output_size);
size_t cap_lua_async_collect_active_snapshots(cap_lua_async_job_snapshot_t *out,
                                              size_t max);
size_t cap_lua_async_active_count(void);
const char *cap_lua_job_status_name(cap_lua_job_status_t status);
esp_err_t cap_lua_async_get_status(const char *job_id,
                                   cap_lua_job_status_t *out_status,
                                   char *summary_out,
                                   size_t summary_out_size);
esp_err_t cap_lua_async_wait_settle(const char *job_id,
                                    uint32_t timeout_ms,
                                    cap_lua_job_status_t *out_status,
                                    char *summary_out,
                                    size_t summary_out_size);
