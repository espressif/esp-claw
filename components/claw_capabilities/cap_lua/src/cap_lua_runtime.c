/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_lua_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include "lualib.h"

static const char *TAG = "cap_lua_rt";

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
} cap_lua_exec_ctx_t;

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

static void cap_lua_push_json_value(lua_State *L, const cJSON *item)
{
    cJSON *child = NULL;
    int index = 1;

    if (!item || cJSON_IsNull(item)) {
        lua_pushnil(L);
        return;
    }
    if (cJSON_IsBool(item)) {
        lua_pushboolean(L, cJSON_IsTrue(item));
        return;
    }
    if (cJSON_IsNumber(item)) {
        lua_pushnumber(L, item->valuedouble);
        return;
    }
    if (cJSON_IsString(item)) {
        lua_pushstring(L, item->valuestring);
        return;
    }
    if (cJSON_IsArray(item)) {
        lua_newtable(L);
        cJSON_ArrayForEach(child, item) {
            cap_lua_push_json_value(L, child);
            lua_rawseti(L, -2, index++);
        }
        return;
    }
    if (cJSON_IsObject(item)) {
        lua_newtable(L);
        cJSON_ArrayForEach(child, item) {
            cap_lua_push_json_value(L, child);
            lua_setfield(L, -2, child->string);
        }
        return;
    }

    lua_pushnil(L);
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

    lua_getglobal(L, "__cap_lua_exec_ctx");
    ctx = (cap_lua_exec_ctx_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
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

    lua_getglobal(L, "__cap_lua_exec_ctx");
    ctx = (cap_lua_exec_ctx_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx && ctx->stop_requested && *ctx->stop_requested;
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

static void cap_lua_set_args_global(lua_State *L, const char *args_json)
{
    cJSON *root = NULL;

    if (args_json && args_json[0]) {
        root = cJSON_Parse(args_json);
    }

    if (root) {
        cap_lua_push_json_value(L, root);
        cJSON_Delete(root);
    } else {
        lua_newtable(L);
    }

    lua_setglobal(L, "args");
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

static void cap_lua_add_script_dir_to_package_path(lua_State *L, const char *script_path)
{
    const char *current_path = NULL;
    const char *last_slash = NULL;
    char script_dir[CAP_LUA_JOB_PATH_MAX] = {0};
    char package_path[2048] = {0};
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

    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    lua_getfield(L, -1, "path");
    current_path = lua_tostring(L, -1);

    /* The running script's own directory has priority over configured shared Lua library directories. */
    written = snprintf(package_path,
                       sizeof(package_path),
                       "%s/?.lua;%s/?/init.lua",
                       script_dir,
                       script_dir);
    if (written < 0 || (size_t)written >= sizeof(package_path)) {
        ESP_LOGW(TAG, "Lua package.path is too long for script: %s", script_path);
        lua_pop(L, 1);
        lua_pop(L, 1);
        return;
    }
    offset = (size_t)written;

    dir_count = cap_lua_get_package_path_dir_count();
    for (i = 0; i < dir_count; i++) {
        const char *dir = cap_lua_get_package_path_dir(i);

        if (!dir || !dir[0]) {
            continue;
        }
        written = snprintf(package_path + offset,
                           sizeof(package_path) - offset,
                           ";%s/?.lua;%s/?/init.lua",
                           dir,
                           dir);
        if (written < 0 || (size_t)written >= sizeof(package_path) - offset) {
            ESP_LOGW(TAG, "Lua package.path is too long after adding shared dir: %s", dir);
            lua_pop(L, 1);
            lua_pop(L, 1);
            return;
        }
        offset += (size_t)written;
    }

    written = snprintf(package_path + offset,
                       sizeof(package_path) - offset,
                       "%s%s",
                       current_path && current_path[0] ? ";" : "",
                       current_path ? current_path : "");
    lua_pop(L, 1);
    if (written < 0 || (size_t)written >= sizeof(package_path) - offset) {
        ESP_LOGW(TAG, "Lua package.path is too long for script: %s", script_path);
        lua_pop(L, 1);
        return;
    }

    lua_pushstring(L, package_path);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);
}

esp_err_t cap_lua_runtime_init(void)
{
    ESP_LOGI(TAG,
             "Lua runtime ready: scripts=%s registered_modules=%u",
             cap_lua_get_base_dir(),
             (unsigned int)cap_lua_get_module_count());
    return ESP_OK;
}

esp_err_t cap_lua_runtime_execute_file(const char *path,
                                       const char *args_json,
                                       uint32_t timeout_ms,
                                       volatile bool *stop_requested,
                                       cap_lua_runtime_log_fn_t log_fn,
                                       void *log_ctx,
                                       char *output,
                                       size_t output_size)
{
    struct stat st = {0};
    lua_State *L = NULL;
    cap_lua_exec_ctx_t ctx = {
        .buf = output,
        .size = output_size,
        .deadline_us = (timeout_ms == 0)
                       ? 0
                       : esp_timer_get_time() + ((int64_t)timeout_ms * 1000),
        .stop_requested = stop_requested,
        .log_fn = log_fn,
        .log_ctx = log_ctx,
    };
    int status;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    if (!cap_lua_run_path_is_valid(path)) {
        snprintf(output, output_size, "Error: Lua path must be a valid .lua script path");
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(path, &st) != 0) {
        snprintf(output, output_size, "Error: Lua script not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0 || st.st_size > CAP_LUA_MAX_SCRIPT_SIZE) {
        snprintf(output, output_size, "Error: Lua script size invalid: %ld bytes", (long)st.st_size);
        return ESP_ERR_INVALID_SIZE;
    }

    L = luaL_newstate();
    if (!L) {
        snprintf(output, output_size, "Error: failed to create Lua state");
        return ESP_ERR_NO_MEM;
    }

    luaL_openlibs(L);
    cap_lua_load_registered_modules(L);
    cap_lua_add_script_dir_to_package_path(L, path);
    lua_pushlightuserdata(L, &ctx);
    lua_setglobal(L, "__cap_lua_exec_ctx");
    cap_lua_set_args_global(L, args_json);
    lua_pushlightuserdata(L, &ctx);
    lua_pushcclosure(L, cap_lua_print_capture, 1);
    lua_setglobal(L, "print");
    lua_sethook(L, cap_lua_timeout_hook, LUA_MASKCOUNT, 100);

    status = luaL_dofile(L, path);
    cap_lua_run_exit_cleanups(L);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        if (ctx.len > 0) {
            cap_lua_output_append(&ctx, "ERROR: ", 7);
        }
        cap_lua_output_append(&ctx,
                              msg ? msg : "unknown Lua error",
                              strlen(msg ? msg : "unknown Lua error"));
        cap_lua_output_append(&ctx, "\n", 1);
        lua_close(L);
        return ESP_FAIL;
    }

    if (ctx.len == 0) {
        cap_lua_output_append(&ctx, "Lua script completed with no output.\n", 36);
    } else if (ctx.truncated) {
        cap_lua_output_append(&ctx, "[output truncated]\n", 19);
    }

    lua_close(L);
    return ESP_OK;
}
