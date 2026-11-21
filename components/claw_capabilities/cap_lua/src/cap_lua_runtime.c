/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_lua_internal.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "cap_lua_hw.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lualib.h"

static const char *TAG = "cap_lua_rt";

/* "lua/job/" + up to 10 decimal digits + NUL, rounded up for headroom. */
#define CAP_LUA_OWNER_TAG_MAX 24

typedef struct {
    char *buf;
    size_t size;
    size_t len;
    bool truncated;
    /* deadline_us == 0 means "no wall-clock deadline" (cancel-only mode). */
    int64_t deadline_us;
    volatile bool *stop_requested;
    cap_lua_runtime_log_fn_t log_fn;
    void *log_ctx;
    char job_id[CAP_LUA_JOB_ID_LEN];
    /* Owner tag handed out by cap_lua_current_owner_tag(); must outlive
     * every C callback that runs on this task. */
    char owner_tag[CAP_LUA_OWNER_TAG_MAX];
} cap_lua_exec_ctx_t;

/* task -> owner_tag stack with one slot per possible concurrent Lua job.
 * A dedicated table (rather than FreeRTOS TLS) avoids fighting the app's
 * scarce TLS pointer slots. Each nested runtime invocation owns one frame so
 * returning from an inner invocation restores the outer owner tag. */
typedef struct cap_lua_owner_tag_frame {
    const char *tag;
    struct cap_lua_owner_tag_frame *prev;
} cap_lua_owner_tag_frame_t;

typedef struct {
    TaskHandle_t task;
    cap_lua_owner_tag_frame_t *top;
} cap_lua_owner_tag_slot_t;

static cap_lua_owner_tag_slot_t s_owner_tag_slots[CAP_LUA_ASYNC_MAX_CONCURRENT];
static SemaphoreHandle_t s_owner_tag_lock;
static portMUX_TYPE s_owner_tag_init_lock = portMUX_INITIALIZER_UNLOCKED;

/* Starts at 1 so job 0 is never a valid id. */
static atomic_uint s_owner_tag_next_id = 1;

static esp_err_t cap_lua_ensure_owner_tag_lock(void)
{
    portENTER_CRITICAL(&s_owner_tag_init_lock);
    bool ready = s_owner_tag_lock != NULL;
    portEXIT_CRITICAL(&s_owner_tag_init_lock);
    if (ready) return ESP_OK;
    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (!candidate) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_owner_tag_init_lock);
    if (!s_owner_tag_lock) {
        s_owner_tag_lock = candidate;
        candidate = NULL;
    }
    portEXIT_CRITICAL(&s_owner_tag_init_lock);
    if (candidate) vSemaphoreDelete(candidate);
    return ESP_OK;
}

static void cap_lua_owner_tag_register(TaskHandle_t task, const char *tag)
{
    cap_lua_owner_tag_frame_t *frame;

    if (!task || !tag || cap_lua_ensure_owner_tag_lock() != ESP_OK) {
        return;
    }
    frame = calloc(1, sizeof(*frame));
    if (!frame) {
        ESP_LOGW(TAG, "failed to allocate owner-tag frame; job will report NULL tag");
        return;
    }
    frame->tag = tag;
    if (xSemaphoreTake(s_owner_tag_lock, portMAX_DELAY) != pdTRUE) {
        free(frame);
        return;
    }
    int free_slot = -1;
    for (int i = 0; i < CAP_LUA_ASYNC_MAX_CONCURRENT; i++) {
        if (s_owner_tag_slots[i].task == task) {
            frame->prev = s_owner_tag_slots[i].top;
            s_owner_tag_slots[i].top = frame;
            xSemaphoreGive(s_owner_tag_lock);
            return;
        }
        if (free_slot < 0 && s_owner_tag_slots[i].task == NULL) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        ESP_LOGW(TAG, "owner-tag slot table full; job will report NULL tag");
        xSemaphoreGive(s_owner_tag_lock);
        free(frame);
        return;
    }
    s_owner_tag_slots[free_slot].task = task;
    s_owner_tag_slots[free_slot].top = frame;
    xSemaphoreGive(s_owner_tag_lock);
}

static void cap_lua_owner_tag_unregister(TaskHandle_t task, const char *tag)
{
    if (!task || !s_owner_tag_lock) {
        return;
    }
    if (xSemaphoreTake(s_owner_tag_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (int i = 0; i < CAP_LUA_ASYNC_MAX_CONCURRENT; i++) {
        if (s_owner_tag_slots[i].task == task) {
            cap_lua_owner_tag_frame_t **cursor = &s_owner_tag_slots[i].top;
            while (*cursor != NULL) {
                cap_lua_owner_tag_frame_t *frame = *cursor;
                if (frame->tag == tag) {
                    *cursor = frame->prev;
                    free(frame);
                    if (s_owner_tag_slots[i].top == NULL) {
                        s_owner_tag_slots[i].task = NULL;
                    }
                    xSemaphoreGive(s_owner_tag_lock);
                    return;
                }
                cursor = &frame->prev;
            }
            break;
        }
    }
    xSemaphoreGive(s_owner_tag_lock);
}

const char *cap_lua_current_owner_tag(void)
{
    TaskHandle_t task;
    const char *tag = NULL;

    if (!s_owner_tag_lock) {
        return NULL;
    }
    task = xTaskGetCurrentTaskHandle();
    if (!task) {
        return NULL;
    }
    if (xSemaphoreTake(s_owner_tag_lock, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }
    for (int i = 0; i < CAP_LUA_ASYNC_MAX_CONCURRENT; i++) {
        if (s_owner_tag_slots[i].task == task) {
            tag = s_owner_tag_slots[i].top ? s_owner_tag_slots[i].top->tag : NULL;
            break;
        }
    }
    xSemaphoreGive(s_owner_tag_lock);
    return tag;
}

static void cap_lua_output_append(cap_lua_exec_ctx_t *ctx,
                                  const char *text,
                                  size_t len)
{
    size_t room;
    size_t copy;

    if (!ctx || !ctx->buf || ctx->size == 0 || !text || len == 0) {
        return;
    }

    if (ctx->len >= ctx->size - 1) {
        ctx->truncated = true;
        return;
    }

    room = ctx->size - 1 - ctx->len;
    copy = len < room ? len : room;
    memcpy(ctx->buf + ctx->len, text, copy);
    ctx->len += copy;
    ctx->buf[ctx->len] = '\0';
    if (copy < len) {
        ctx->truncated = true;
    }
}

/* Extra space keeps the context private and is inherited by Lua coroutines. */
static void cap_lua_set_exec_ctx(lua_State *L, cap_lua_exec_ctx_t *ctx)
{
    *(cap_lua_exec_ctx_t **)lua_getextraspace(L) = ctx;
}

static cap_lua_exec_ctx_t *cap_lua_get_exec_ctx(lua_State *L)
{
    return *(cap_lua_exec_ctx_t **)lua_getextraspace(L);
}

/* cJSON's parser allows deep nesting (default limit ~1000); recursing that far
 * on the small Lua task stack would overflow it. Cap the depth of the args ->
 * Lua-table conversion well below any realistic legitimate payload. */
#define CAP_LUA_JSON_MAX_DEPTH 64

/* Converts one cJSON value into a Lua value pushed on top of L's stack.
 * Returns ESP_OK on success (exactly one value pushed), or
 * ESP_ERR_INVALID_SIZE when the nesting exceeds CAP_LUA_JSON_MAX_DEPTH. On
 * error nothing extra is left on the stack: any partial table built for the
 * current level is popped before returning so the failure can be surfaced to
 * the caller rather than silently truncating the args. */
static esp_err_t cap_lua_push_json_value_depth(lua_State *L, const cJSON *item, int depth)
{
    cJSON *child = NULL;
    int index = 1;

    if (!item || cJSON_IsNull(item)) {
        lua_pushnil(L);
        return ESP_OK;
    }
    if (cJSON_IsBool(item)) {
        lua_pushboolean(L, cJSON_IsTrue(item));
        return ESP_OK;
    }
    if (cJSON_IsNumber(item)) {
        lua_pushnumber(L, item->valuedouble);
        return ESP_OK;
    }
    if (cJSON_IsString(item)) {
        lua_pushstring(L, item->valuestring);
        return ESP_OK;
    }
    if (depth >= CAP_LUA_JSON_MAX_DEPTH) {
        /* Too deep: refuse to descend further rather than risk a C-stack
         * overflow on the constrained Lua task stack. Report the error so the
         * caller can fail the run with a diagnosable message instead of
         * silently dropping the over-deep portion of the args. */
        return ESP_ERR_INVALID_SIZE;
    }
    if (cJSON_IsArray(item)) {
        lua_newtable(L);
        cJSON_ArrayForEach(child, item) {
            esp_err_t err = cap_lua_push_json_value_depth(L, child, depth + 1);
            if (err != ESP_OK) {
                lua_pop(L, 1);
                return err;
            }
            lua_rawseti(L, -2, index++);
        }
        return ESP_OK;
    }
    if (cJSON_IsObject(item)) {
        lua_newtable(L);
        cJSON_ArrayForEach(child, item) {
            esp_err_t err = cap_lua_push_json_value_depth(L, child, depth + 1);
            if (err != ESP_OK) {
                lua_pop(L, 1);
                return err;
            }
            lua_setfield(L, -2, child->string);
        }
        return ESP_OK;
    }

    lua_pushnil(L);
    return ESP_OK;
}

static esp_err_t cap_lua_push_json_value(lua_State *L, const cJSON *item)
{
    return cap_lua_push_json_value_depth(L, item, 0);
}

static int cap_lua_print_capture(lua_State *L)
{
    cap_lua_exec_ctx_t *ctx = (cap_lua_exec_ctx_t *)lua_touserdata(
                                  L, lua_upvalueindex(1));
    int top = lua_gettop(L);
    int i;

    for (i = 1; i <= top; i++) {
        size_t len = 0;
        const char *text = luaL_tolstring(L, i, &len);

        if (i > 1) {
            cap_lua_output_append(ctx, "\t", 1);
            if (ctx && ctx->log_fn) {
                ctx->log_fn(ctx->log_ctx, "\t", 1);
            }
            fwrite("\t", sizeof(char), 1, stdout);
        }
        cap_lua_output_append(ctx, text, len);
        if (ctx && ctx->log_fn) {
            ctx->log_fn(ctx->log_ctx, text, len);
        }
        fwrite(text, sizeof(char), len, stdout);
        lua_pop(L, 1);
    }

    cap_lua_output_append(ctx, "\n", 1);
    if (ctx && ctx->log_fn) {
        ctx->log_fn(ctx->log_ctx, "\n", 1);
    }
    fwrite("\n", sizeof(char), 1, stdout);
    fflush(stdout);
    return 0;
}

static void cap_lua_timeout_hook(lua_State *L, lua_Debug *ar)
{
    cap_lua_exec_ctx_t *ctx = NULL;

    (void)ar;

    ctx = cap_lua_get_exec_ctx(L);
    if (!ctx) {
        return;
    }

    /* Cooperative cancellation always wins over deadline reporting. */
    if (ctx->stop_requested && *ctx->stop_requested) {
        luaL_error(L, "stopped by user");
    }

    if (ctx->deadline_us != 0 && esp_timer_get_time() > ctx->deadline_us) {
        luaL_error(L, "execution timed out");
    }
}

bool cap_lua_runtime_stop_requested(lua_State *L)
{
    cap_lua_exec_ctx_t *ctx = NULL;

    if (!L) {
        return false;
    }

    ctx = cap_lua_get_exec_ctx(L);
    return ctx && ctx->stop_requested && *ctx->stop_requested;
}

bool cap_lua_runtime_request_stop(lua_State *L)
{
    cap_lua_exec_ctx_t *ctx = NULL;

    if (!L) {
        return false;
    }

    ctx = cap_lua_get_exec_ctx(L);
    if (!ctx || !ctx->stop_requested) {
        return false;
    }
    *ctx->stop_requested = true;
    return true;
}

const char *cap_lua_runtime_job_id(lua_State *L)
{
    cap_lua_exec_ctx_t *ctx = cap_lua_get_exec_ctx(L);
    return ctx && ctx->job_id[0] ? ctx->job_id : NULL;
}

static void cap_lua_load_registered_modules(lua_State *L)
{
    size_t i;

    for (i = 0; i < cap_lua_get_module_count(); i++) {
        const cap_lua_module_t *module = cap_lua_get_module(i);

        if (!module || !module->name || !module->open_fn) {
            continue;
        }

        luaL_requiref(L, module->name, module->open_fn, 1);
        lua_pop(L, 1);
    }
}

static esp_err_t cap_lua_set_args_global(lua_State *L, const char *args_json)
{
    cJSON *root = NULL;
    esp_err_t err = ESP_OK;

    if (args_json && args_json[0]) {
        root = cJSON_Parse(args_json);
    }

    if (root) {
        err = cap_lua_push_json_value(L, root);
        cJSON_Delete(root);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        lua_newtable(L);
    }

    lua_setglobal(L, "args");
    return ESP_OK;
}

static void cap_lua_run_exit_cleanups(lua_State *L)
{
    size_t i;

    for (i = 0; i < cap_lua_get_exit_cleanup_count(); i++) {
        cap_lua_exit_cleanup_fn_t cleanup_fn = cap_lua_get_exit_cleanup(i);

        if (cleanup_fn) {
            cleanup_fn(L);
        }
    }
}

#define CAP_LUA_PACKAGE_PATH_MAX 2048

static void cap_lua_add_script_dir_to_package_path(lua_State *L, const char *script_path)
{
    const char *current_path = NULL;
    const char *last_slash = NULL;
    char script_dir[CAP_LUA_JOB_PATH_MAX] = {0};
    /* Keep this assembled string off the call stack: the function may be
     * reached from the agent main task whose stack is far smaller than 2 KB. */
    char *package_path = NULL;
    size_t offset = 0;
    size_t dir_count;
    size_t i;
    int written;

    if (!L || !script_path || !script_path[0]) {
        return;
    }

    last_slash = strrchr(script_path, '/');
    if (!last_slash) {
        return;
    }

    written = snprintf(script_dir, sizeof(script_dir), "%.*s", (int)(last_slash - script_path), script_path);
    if (written < 0 || (size_t)written >= sizeof(script_dir)) {
        ESP_LOGW(TAG, "Script directory path is too long: %s", script_path);
        return;
    }

    package_path = malloc(CAP_LUA_PACKAGE_PATH_MAX);
    if (!package_path) {
        ESP_LOGW(TAG, "Failed to allocate Lua package.path buffer for script: %s", script_path);
        return;
    }

    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        goto cleanup;
    }

    lua_getfield(L, -1, "path");
    current_path = lua_tostring(L, -1);

    /* The running script's own directory has priority over configured shared Lua library directories. */
    written = snprintf(package_path,
                       CAP_LUA_PACKAGE_PATH_MAX,
                       "%s/?.lua;%s/?/init.lua",
                       script_dir,
                       script_dir);
    if (written < 0 || (size_t)written >= CAP_LUA_PACKAGE_PATH_MAX) {
        ESP_LOGW(TAG, "Lua package.path is too long for script: %s", script_path);
        lua_pop(L, 1);
        lua_pop(L, 1);
        goto cleanup;
    }
    offset = (size_t)written;

    dir_count = cap_lua_get_package_path_dir_count();
    for (i = 0; i < dir_count; i++) {
        const char *dir = cap_lua_get_package_path_dir(i);

        if (!dir || !dir[0]) {
            continue;
        }
        written = snprintf(package_path + offset,
                           CAP_LUA_PACKAGE_PATH_MAX - offset,
                           ";%s/?.lua;%s/?/init.lua",
                           dir,
                           dir);
        if (written < 0 || (size_t)written >= CAP_LUA_PACKAGE_PATH_MAX - offset) {
            ESP_LOGW(TAG, "Lua package.path is too long after adding shared dir: %s", dir);
            lua_pop(L, 1);
            lua_pop(L, 1);
            goto cleanup;
        }
        offset += (size_t)written;
    }

    written = snprintf(package_path + offset,
                       CAP_LUA_PACKAGE_PATH_MAX - offset,
                       "%s%s",
                       current_path && current_path[0] ? ";" : "",
                       current_path ? current_path : "");
    lua_pop(L, 1);
    if (written < 0 || (size_t)written >= CAP_LUA_PACKAGE_PATH_MAX - offset) {
        ESP_LOGW(TAG, "Lua package.path is too long for script: %s", script_path);
        lua_pop(L, 1);
        goto cleanup;
    }

    lua_pushstring(L, package_path);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

cleanup:
    free(package_path);
}

/* Keep diagnostic helpers without exposing C userdata, registry entries, or hooks. */
static void cap_lua_restrict_debug(lua_State *L)
{
    lua_getglobal(L, "debug");
    lua_newtable(L);
    lua_getfield(L, -2, "traceback");
    lua_setfield(L, -2, "traceback");
    lua_getfield(L, -2, "getinfo");
    lua_setfield(L, -2, "getinfo");
    lua_pushvalue(L, -1);
    lua_setglobal(L, "debug");
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "debug");
    lua_pop(L, 3);
}

esp_err_t cap_lua_runtime_init(void)
{
    esp_err_t err = cap_lua_ensure_owner_tag_lock();
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG,"Lua runtime ready: registered_modules=%u", (unsigned int)cap_lua_get_module_count());
    return ESP_OK;
}

esp_err_t cap_lua_runtime_execute_file(const char *path,
                                       const char *args_json,
                                       const char *job_id,
                                       uint32_t timeout_ms,
                                       volatile bool *stop_requested,
                                       cap_lua_runtime_log_fn_t log_fn,
                                       void *log_ctx,
                                       char *output,
                                       size_t output_size)
{
    struct stat st = {0};
    lua_State *L = NULL;
    TaskHandle_t owning_task;
    unsigned job_seq;
    int status;
    esp_err_t result_err = ESP_OK;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';
    if (!job_id || !job_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cap_lua_ensure_owner_tag_lock() != ESP_OK) {
        snprintf(output, output_size, "Error: failed to initialize Lua runtime lock");
        return ESP_ERR_NO_MEM;
    }
    cap_lua_exec_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        snprintf(output, output_size, "Error: failed to allocate Lua runtime context");
        return ESP_ERR_NO_MEM;
    }
    ctx->buf = output;
    ctx->size = output_size;
    ctx->deadline_us = timeout_ms ? esp_timer_get_time() + (int64_t)timeout_ms * 1000 : 0;
    ctx->stop_requested = stop_requested;
    ctx->log_fn = log_fn;
    ctx->log_ctx = log_ctx;
    output[0] = '\0';
    strlcpy(ctx->job_id, job_id, sizeof(ctx->job_id));

    /* The owner tag remains valid through all hardware cleanup. */
    owning_task = xTaskGetCurrentTaskHandle();
    job_seq = (unsigned)atomic_fetch_add(&s_owner_tag_next_id, 1);
    snprintf(ctx->owner_tag, sizeof(ctx->owner_tag), "lua/job/%u", job_seq);
    cap_lua_owner_tag_register(owning_task, ctx->owner_tag);

    if (!cap_lua_run_path_is_valid(path)) {
        snprintf(output, output_size, "Error: Lua path must be a valid .lua script path");
        result_err = ESP_ERR_INVALID_ARG;
        goto cleanup_tag_only;
    }

    if (stat(path, &st) != 0) {
        snprintf(output, output_size, "Error: Lua script not found: %s", path);
        result_err = ESP_ERR_NOT_FOUND;
        goto cleanup_tag_only;
    }
    if (st.st_size <= 0 || st.st_size > CAP_LUA_MAX_SCRIPT_SIZE) {
        snprintf(output, output_size, "Error: Lua script size invalid: %ld bytes", (long)st.st_size);
        result_err = ESP_ERR_INVALID_SIZE;
        goto cleanup_tag_only;
    }

    L = luaL_newstate();
    if (!L) {
        snprintf(output, output_size, "Error: failed to create Lua state");
        result_err = ESP_ERR_NO_MEM;
        goto cleanup_tag_only;
    }

    cap_lua_hw_bind(L, cap_lua_get_hw_registry());
    luaL_openlibs(L);
    cap_lua_restrict_debug(L);
    cap_lua_load_registered_modules(L);
    cap_lua_add_script_dir_to_package_path(L, path);
    cap_lua_set_exec_ctx(L, ctx);
    if (cap_lua_set_args_global(L, args_json) != ESP_OK) {
        snprintf(output, output_size,
                 "Error: Lua args JSON nesting exceeds %d levels",
                 CAP_LUA_JSON_MAX_DEPTH);
        result_err = ESP_ERR_INVALID_SIZE;
        goto cleanup_full;
    }
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, cap_lua_print_capture, 1);
    lua_setglobal(L, "print");
    lua_sethook(L, cap_lua_timeout_hook, LUA_MASKCOUNT, 100);

    status = luaL_dofile(L, path);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        if (ctx->len > 0) {
            cap_lua_output_append(ctx, "ERROR: ", 7);
        }
        cap_lua_output_append(ctx,
                              msg ? msg : "unknown Lua error",
                              strlen(msg ? msg : "unknown Lua error"));
        cap_lua_output_append(ctx, "\n", 1);
        result_err = ESP_FAIL;
        goto cleanup_full;
    }

    if (ctx->len == 0) {
        cap_lua_output_append(ctx, "Lua script completed with no output.\n", 36);
    } else if (ctx->truncated) {
        cap_lua_output_append(ctx, "[output truncated]\n", 19);
    }

cleanup_full:
    /* Finalizers may close existing handles, but cannot acquire new hardware. */
    cap_lua_hw_bind(L, NULL);
    lua_sethook(L, NULL, 0, 0);
    cap_lua_run_exit_cleanups(L);
    lua_close(L);
cleanup_tag_only:
    cap_lua_owner_tag_unregister(owning_task, ctx->owner_tag);
    free(ctx);
    return result_err;
}
