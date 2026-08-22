/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "claw_core.h"
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    lua_CFunction open_fn;
} cap_lua_module_t;

typedef void (*cap_lua_exit_cleanup_fn_t)(lua_State *L);

#define CAP_LUA_JOB_NAME_MAX            32
#define CAP_LUA_JOB_EXCLUSIVE_MAX       32
#define CAP_LUA_JOB_PATH_MAX            192
#define CAP_LUA_JOB_ID_LEN              9

typedef enum {
    CAP_LUA_JOB_QUEUED = 0,
    CAP_LUA_JOB_RUNNING,
    CAP_LUA_JOB_DONE,
    CAP_LUA_JOB_FAILED,
    CAP_LUA_JOB_TIMEOUT,
    CAP_LUA_JOB_STOPPED,
} cap_lua_job_status_t;

typedef enum {
    CAP_LUA_JOB_EVENT_CREATED = 0,
    CAP_LUA_JOB_EVENT_RUNNING,
    CAP_LUA_JOB_EVENT_STOP_REQUESTED,
    CAP_LUA_JOB_EVENT_TERMINAL,
} cap_lua_job_event_type_t;

typedef struct {
    cap_lua_job_event_type_t type;
    cap_lua_job_status_t status;
    char job_id[CAP_LUA_JOB_ID_LEN];
    char name[CAP_LUA_JOB_NAME_MAX];
    char exclusive[CAP_LUA_JOB_EXCLUSIVE_MAX];
    char path[CAP_LUA_JOB_PATH_MAX];
} cap_lua_job_event_t;

typedef void (*cap_lua_job_event_cb_t)(const cap_lua_job_event_t *event, void *user_ctx);

esp_err_t cap_lua_register_group(void);
esp_err_t cap_lua_add_package_path_dir(const char *dir);
esp_err_t cap_lua_register_module(const char *name, lua_CFunction open_fn);
esp_err_t cap_lua_register_modules(const cap_lua_module_t *modules, size_t count);
esp_err_t cap_lua_register_exit_cleanup(cap_lua_exit_cleanup_fn_t cleanup_fn);
esp_err_t cap_lua_register_job_event_cb(cap_lua_job_event_cb_t cb, void *user_ctx);
esp_err_t cap_lua_unregister_job_event_cb(cap_lua_job_event_cb_t cb, void *user_ctx);
bool cap_lua_runtime_stop_requested(lua_State *L);
esp_err_t cap_lua_run_script(const char *path,
                             const char *args_json,
                             uint32_t timeout_ms,
                             char *output,
                             size_t output_size);

esp_err_t cap_lua_run_script_async(const char *path,
                                   const char *args_json,
                                   uint32_t timeout_ms,
                                   const char *name,
                                   const char *exclusive,
                                   bool replace,
                                   char *output,
                                   size_t output_size);
esp_err_t cap_lua_list_jobs(const char *status, char *output, size_t output_size);
esp_err_t cap_lua_get_job(const char *id_or_name, char *output, size_t output_size);
esp_err_t cap_lua_stop_job(const char *id_or_name,
                           uint32_t wait_ms,
                           char *output,
                           size_t output_size);
esp_err_t cap_lua_stop_all_jobs(const char *exclusive_filter,
                                uint32_t wait_ms,
                                char *output,
                                size_t output_size);

size_t cap_lua_get_active_async_job_count(void);

#ifdef __cplusplus
}
#endif
