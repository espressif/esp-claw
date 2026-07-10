/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cap_mcp_server.h"
#include "cap_lua.h"

#define CAP_MCP_SERVER_LUA_BUF         4096
#define CAP_MCP_SERVER_LUA_SCRIPTS_DIR "/fatfs/scripts"

typedef enum {
    CAP_MCP_SERVER_LUA_RUN_SCRIPT,
    CAP_MCP_SERVER_LUA_RUN_SCRIPT_ASYNC,
    CAP_MCP_SERVER_LUA_LIST_JOBS,
    CAP_MCP_SERVER_LUA_GET_JOB,
    CAP_MCP_SERVER_LUA_STOP_JOB,
    CAP_MCP_SERVER_LUA_STOP_ALL_JOBS,
} cap_mcp_server_lua_op_t;

typedef struct {
    const char *path;
    const char *args_json;
    const char *timeout_ms_str;
    const char *name;
    const char *exclusive;
    const char *replace_str;
    const char *status;
    const char *job_id;
    const char *job_name;
    const char *wait_ms_str;
    const char *exclusive_filter;
} cap_mcp_server_lua_args_t;

static bool cap_mcp_server_lua_parse_bool_default_false(const char *s)
{
    if (!s || !s[0]) {
        return false;
    }
    if (strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0) {
        return true;
    }
    return false;
}

static uint32_t cap_mcp_server_lua_parse_u32(const char *s)
{
    if (!s || !s[0]) {
        return 0;
    }
    return (uint32_t)strtoul(s, NULL, 10);
}

static const char *cap_mcp_server_lua_job_id_or_name(const cap_mcp_server_lua_args_t *args)
{
    if (args->job_id && args->job_id[0]) {
        return args->job_id;
    }
    return args->job_name;
}

static bool cap_mcp_server_lua_run_path_is_valid(const char *path)
{
    size_t path_len;

    if (!path || !path[0] || strstr(path, "..") != NULL) {
        return false;
    }

    path_len = strlen(path);
    return path_len > 4 && strcmp(path + path_len - 4, ".lua") == 0;
}

static esp_err_t cap_mcp_server_lua_resolve_run_path(const char *path, char *resolved, size_t resolved_size)
{
    int written;

    if (!path || !path[0] || !resolved || resolved_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path[0] == '/') {
        if (!cap_mcp_server_lua_run_path_is_valid(path)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (strlcpy(resolved, path, resolved_size) >= resolved_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }
    if (!cap_mcp_server_lua_run_path_is_valid(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(resolved, resolved_size, "%s/%s", CAP_MCP_SERVER_LUA_SCRIPTS_DIR, path);
    if (written < 0 || (size_t)written >= resolved_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t cap_mcp_server_lua_invoke(cap_mcp_server_lua_op_t op,
                                             const cap_mcp_server_lua_args_t *args,
                                             char *output,
                                             size_t output_size)
{
    const cap_mcp_server_lua_args_t a = args ? *args : (cap_mcp_server_lua_args_t){0};

    switch (op) {
    case CAP_MCP_SERVER_LUA_RUN_SCRIPT: {
        char run_path[384];

        if (cap_mcp_server_lua_resolve_run_path(a.path, run_path, sizeof(run_path)) != ESP_OK) {
            snprintf(output, output_size, "Error: path must be a relative or absolute .lua path");
            return ESP_ERR_INVALID_ARG;
        }
        return cap_lua_run_script(run_path,
                                  a.args_json,
                                  cap_mcp_server_lua_parse_u32(a.timeout_ms_str),
                                  output,
                                  output_size);
    }
    case CAP_MCP_SERVER_LUA_RUN_SCRIPT_ASYNC: {
        char run_path[384];

        if (cap_mcp_server_lua_resolve_run_path(a.path, run_path, sizeof(run_path)) != ESP_OK) {
            snprintf(output, output_size, "Error: path must be a relative or absolute .lua path");
            return ESP_ERR_INVALID_ARG;
        }
        return cap_lua_run_script_async(run_path,
                                        a.args_json,
                                        cap_mcp_server_lua_parse_u32(a.timeout_ms_str),
                                        a.name,
                                        a.exclusive,
                                        cap_mcp_server_lua_parse_bool_default_false(a.replace_str),
                                        output,
                                        output_size);
    }
    case CAP_MCP_SERVER_LUA_LIST_JOBS:
        return cap_lua_list_jobs(a.status, output, output_size);
    case CAP_MCP_SERVER_LUA_GET_JOB:
        return cap_lua_get_job(cap_mcp_server_lua_job_id_or_name(&a), output, output_size);
    case CAP_MCP_SERVER_LUA_STOP_JOB:
        return cap_lua_stop_job(cap_mcp_server_lua_job_id_or_name(&a),
                                cap_mcp_server_lua_parse_u32(a.wait_ms_str),
                                output,
                                output_size);
    case CAP_MCP_SERVER_LUA_STOP_ALL_JOBS:
        return cap_lua_stop_all_jobs(a.exclusive_filter,
                                     cap_mcp_server_lua_parse_u32(a.wait_ms_str),
                                     output,
                                     output_size);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_mcp_value_t cap_mcp_server_lua_to_mcp_value(cap_mcp_server_lua_op_t op,
                                                         const cap_mcp_server_lua_args_t *args)
{
    char *result_str = NULL;
    esp_err_t err;
    esp_mcp_value_t result;

    result_str = calloc(1, CAP_MCP_SERVER_LUA_BUF);
    if (!result_str) {
        return esp_mcp_value_create_bool(false);
    }
    err = cap_mcp_server_lua_invoke(op, args, result_str, CAP_MCP_SERVER_LUA_BUF);
    if (err != ESP_OK) {
        free(result_str);
        return esp_mcp_value_create_bool(false);
    }

    result = esp_mcp_value_create_string(result_str);
    free(result_str);
    return result;
}

static esp_mcp_value_t cap_mcp_server_run_script_callback(
    const esp_mcp_property_list_t *properties)
{
    const char *path = esp_mcp_property_list_get_property_string(properties, "path");
    const char *args_json = esp_mcp_property_list_get_property_string(properties, "args");
    const char *timeout_ms = esp_mcp_property_list_get_property_string(properties, "timeout_ms");
    const cap_mcp_server_lua_args_t args = {
        .path = path,
        .args_json = args_json,
        .timeout_ms_str = timeout_ms,
    };

    return cap_mcp_server_lua_to_mcp_value(CAP_MCP_SERVER_LUA_RUN_SCRIPT, &args);
}

static esp_mcp_value_t cap_mcp_server_run_script_async_callback(
    const esp_mcp_property_list_t *properties)
{
    const char *path = esp_mcp_property_list_get_property_string(properties, "path");
    const char *args_json = esp_mcp_property_list_get_property_string(properties, "args");
    const char *timeout_ms = esp_mcp_property_list_get_property_string(properties, "timeout_ms");
    const char *name = esp_mcp_property_list_get_property_string(properties, "name");
    const char *exclusive = esp_mcp_property_list_get_property_string(properties, "exclusive");
    const char *replace = esp_mcp_property_list_get_property_string(properties, "replace");
    const cap_mcp_server_lua_args_t args = {
        .path = path,
        .args_json = args_json,
        .timeout_ms_str = timeout_ms,
        .name = name,
        .exclusive = exclusive,
        .replace_str = replace,
    };

    return cap_mcp_server_lua_to_mcp_value(CAP_MCP_SERVER_LUA_RUN_SCRIPT_ASYNC, &args);
}

static esp_mcp_value_t cap_mcp_server_list_async_jobs_callback(
    const esp_mcp_property_list_t *properties)
{
    const char *status = esp_mcp_property_list_get_property_string(properties, "status");
    const cap_mcp_server_lua_args_t args = {.status = status};

    return cap_mcp_server_lua_to_mcp_value(CAP_MCP_SERVER_LUA_LIST_JOBS, &args);
}

static esp_mcp_value_t cap_mcp_server_get_async_job_callback(
    const esp_mcp_property_list_t *properties)
{
    const char *job_id = esp_mcp_property_list_get_property_string(properties, "job_id");
    const char *name = esp_mcp_property_list_get_property_string(properties, "name");
    const cap_mcp_server_lua_args_t args = {.job_id = job_id, .job_name = name};

    return cap_mcp_server_lua_to_mcp_value(CAP_MCP_SERVER_LUA_GET_JOB, &args);
}

static esp_mcp_value_t cap_mcp_server_stop_async_job_callback(
    const esp_mcp_property_list_t *properties)
{
    const char *job_id = esp_mcp_property_list_get_property_string(properties, "job_id");
    const char *name = esp_mcp_property_list_get_property_string(properties, "name");
    const char *wait_ms = esp_mcp_property_list_get_property_string(properties, "wait_ms");
    const cap_mcp_server_lua_args_t args = {
        .job_id = job_id,
        .job_name = name,
        .wait_ms_str = wait_ms,
    };

    return cap_mcp_server_lua_to_mcp_value(CAP_MCP_SERVER_LUA_STOP_JOB, &args);
}

static esp_mcp_value_t cap_mcp_server_stop_all_async_jobs_callback(
    const esp_mcp_property_list_t *properties)
{
    const char *exclusive = esp_mcp_property_list_get_property_string(properties, "exclusive");
    const char *wait_ms = esp_mcp_property_list_get_property_string(properties, "wait_ms");
    const cap_mcp_server_lua_args_t args = {
        .exclusive_filter = exclusive,
        .wait_ms_str = wait_ms,
    };

    return cap_mcp_server_lua_to_mcp_value(CAP_MCP_SERVER_LUA_STOP_ALL_JOBS, &args);
}

static const cap_mcp_server_tool_def_t s_tool_defs[] = {
    {
        .name = "lua.run_script",
        .description =
        "Run a Lua script synchronously. path may be relative to scripts/ (for example temp/foo.lua) "
        "or an absolute .lua path.",
        .callback = cap_mcp_server_run_script_callback,
        .property_names = {"path", "args", "timeout_ms"},
        .property_count = 3,
    },
    {
        .name = "lua.run_script_async",
        .description =
        "Run a Lua script asynchronously; path may be relative to scripts/ or absolute. Returns a job id. "
        "timeout_ms=0 means run until cancelled (default). Use 'name' to label, 'exclusive' "
        "for mutex groups (e.g. 'display'), 'replace':true to take over a conflicting slot.",
        .callback = cap_mcp_server_run_script_async_callback,
        .property_names = {"path", "args", "timeout_ms", "name", "exclusive", "replace"},
        .property_count = 6,
    },
    {
        .name = "lua.list_async_jobs",
        .description = "List Lua async jobs by optional status filter.",
        .callback = cap_mcp_server_list_async_jobs_callback,
        .property_names = {"status"},
        .property_count = 1,
    },
    {
        .name = "lua.get_async_job",
        .description = "Get the status and summary for a Lua async job by job_id or name.",
        .callback = cap_mcp_server_get_async_job_callback,
        .property_names = {"job_id", "name"},
        .property_count = 2,
    },
    {
        .name = "lua.stop_async_job",
        .description =
        "Stop a running Lua async job by job_id or name. MUST be called whenever the user asks "
        "to stop, cancel, quit or close an async script; replying without calling this leaves "
        "the job running. Cooperative; default wait 2000 ms.",
        .callback = cap_mcp_server_stop_async_job_callback,
        .property_names = {"job_id", "name", "wait_ms"},
        .property_count = 3,
    },
    {
        .name = "lua.stop_all_async_jobs",
        .description =
        "Stop all running Lua async jobs, optionally filtered by exclusive group "
        "(e.g. exclusive='display'). MUST be called when the user asks to clear the screen, "
        "stop everything or cancel all background scripts.",
        .callback = cap_mcp_server_stop_all_async_jobs_callback,
        .property_names = {"exclusive", "wait_ms"},
        .property_count = 2,
    },
};

esp_err_t cap_mcp_lua_tools_init(void)
{
    return cap_mcp_server_add_tool(s_tool_defs, sizeof(s_tool_defs) / sizeof(s_tool_defs[0]));
}
