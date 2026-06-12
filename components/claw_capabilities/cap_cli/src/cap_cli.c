/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_cap.h"
#include "claw_utils_string.h"
#include "cJSON.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    char *command_name;
    char *description;
    char *usage_hint;
} cap_cli_entry_t;

typedef struct {
    bool initialized;
    size_t capacity;
    size_t count;
    size_t max_output_bytes;
    SemaphoreHandle_t mutex;
    cap_cli_entry_t *entries;
    char tool_description[1024];
} cap_cli_state_t;

static const char *TAG = "cap_cli";
#define CAP_CLI_INPUT_SCHEMA_JSON \
    "{\"type\":\"object\",\"properties\":{\"command_line\":{\"type\":\"string\"," \
    "\"description\":\"Full ESP console CLI command line to execute. The first token must match an allowed command.\"}}," \
    "\"required\":[\"command_line\"]}"

static EXT_RAM_BSS_ATTR cap_cli_state_t s_cli = {0};

static char *cap_cli_strdup(const char *src)
{
    if (!src) {
        return NULL;
    }

    return strdup(src);
}

static void cap_cli_free_entry(cap_cli_entry_t *entry)
{
    if (!entry) {
        return;
    }

    free(entry->command_name);
    free(entry->description);
    free(entry->usage_hint);
    memset(entry, 0, sizeof(*entry));
}

static int cap_cli_find_command_locked(const char *command_name)
{
    size_t i;

    if (!command_name || !command_name[0]) {
        return -1;
    }

    for (i = 0; i < s_cli.count; i++) {
        if (s_cli.entries[i].command_name &&
                strcmp(s_cli.entries[i].command_name, command_name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static void cap_cli_refresh_description_locked(void)
{
    size_t i;
    size_t len = 0;

    len = strlcpy(s_cli.tool_description,
                  "Run one allowed ESP console CLI command. Allowed commands:",
                  sizeof(s_cli.tool_description));
    if (len >= sizeof(s_cli.tool_description)) {
        s_cli.tool_description[sizeof(s_cli.tool_description) - 1] = '\0';
        return;
    }

    for (i = 0; i < s_cli.count; i++) {
        const cap_cli_entry_t *entry = &s_cli.entries[i];
        int written;

        if (!entry->command_name || !entry->command_name[0]) {
            continue;
        }

        written = snprintf(s_cli.tool_description + len,
                           sizeof(s_cli.tool_description) - len,
                           "%s %s%s%s%s%s%s",
                           i == 0 ? "" : ",",
                           entry->command_name,
                           entry->description && entry->description[0] ? " (" : "",
                           entry->description && entry->description[0] ? entry->description : "",
                           entry->usage_hint && entry->usage_hint[0] ? "; use: " : "",
                           entry->usage_hint && entry->usage_hint[0] ? entry->usage_hint : "",
                           ((entry->description && entry->description[0]) ||
                            (entry->usage_hint && entry->usage_hint[0])) ? ")" : "");
        if (written < 0) {
            break;
        }
        if ((size_t)written >= sizeof(s_cli.tool_description) - len) {
            len = sizeof(s_cli.tool_description) - 1;
            break;
        }
        len += (size_t)written;
    }

    if (s_cli.count == 0 && len < sizeof(s_cli.tool_description) - 8) {
        strlcat(s_cli.tool_description, " (none)", sizeof(s_cli.tool_description));
    }
}

static esp_err_t cap_cli_extract_command_line(const char *input_json, char **out_command_line)
{
    cJSON *root = NULL;
    cJSON *command_line = NULL;
    char *copied = NULL;

    if (!input_json || !out_command_line) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_command_line = NULL;

    root = cJSON_Parse(input_json);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    command_line = cJSON_GetObjectItem(root, "command_line");
    if (!cJSON_IsString(command_line) || !command_line->valuestring[0]) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    copied = cap_cli_strdup(command_line->valuestring);
    cJSON_Delete(root);
    if (!copied) {
        return ESP_ERR_NO_MEM;
    }

    *out_command_line = copied;
    return ESP_OK;
}

static esp_err_t cap_cli_parse_command_name(const char *command_line,
                                            char *command_name,
                                            size_t command_name_size)
{
    char *line_copy = NULL;
    char *argv[8] = {0};
    size_t argc;
    esp_err_t err = ESP_OK;

    if (!command_line || !command_line[0] || !command_name || command_name_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    line_copy = cap_cli_strdup(command_line);
    if (!line_copy) {
        return ESP_ERR_NO_MEM;
    }

    argc = esp_console_split_argv(line_copy, argv, sizeof(argv) / sizeof(argv[0]));
    if (argc == 0 || !argv[0] || !argv[0][0]) {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    if (strlcpy(command_name, argv[0], command_name_size) >= command_name_size) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

cleanup:
    free(line_copy);
    return err;
}

static esp_err_t cap_cli_capture_run_locked(const char *command_line,
                                            char **out_stdout_text,
                                            esp_err_t *out_run_err,
                                            int *out_cmd_ret)
{
    FILE *capture = NULL;
    FILE *saved_stdout = NULL;
    char *buffer = NULL;
    size_t buffer_len = 0;

    if (!command_line || !out_stdout_text || !out_run_err || !out_cmd_ret) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_stdout_text = NULL;

    capture = open_memstream(&buffer, &buffer_len);
    if (!capture) {
        return ESP_FAIL;
    }

    fflush(stdout);
    saved_stdout = stdout;
    stdout = capture;

    *out_run_err = esp_console_run(command_line, out_cmd_ret);

    fflush(stdout);
    stdout = saved_stdout;

    if (fclose(capture) != 0) {
        free(buffer);
        return ESP_FAIL;
    }

    if (!buffer) {
        buffer = calloc(1, 1);
        if (!buffer) {
            return ESP_ERR_NO_MEM;
        }
    }

    *out_stdout_text = buffer;
    return ESP_OK;
}

static esp_err_t cap_cli_execute(const char *input_json,
                                 const claw_cap_call_context_t *ctx,
                                 char *output,
                                 size_t output_size)
{
    char *command_line = NULL;
    char *stdout_text = NULL;
    char command_name[64];
    const char *status;
    const char *captured_output;
    const char *truncated_suffix = "";
    size_t captured_len;
    size_t keep_len;
    esp_err_t err;
    esp_err_t run_err = ESP_FAIL;
    int cmd_ret = -1;

    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    err = cap_cli_extract_command_line(input_json ? input_json : "{}", &command_line);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: command_line must be a non-empty string");
        return err;
    }

    err = cap_cli_parse_command_name(command_line, command_name, sizeof(command_name));
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to parse command_line");
        free(command_line);
        return err;
    }

    xSemaphoreTake(s_cli.mutex, portMAX_DELAY);
    if (cap_cli_find_command_locked(command_name) < 0) {
        xSemaphoreGive(s_cli.mutex);
        snprintf(output, output_size, "Error: command is not allowed for cap_cli");
        free(command_line);
        return ESP_ERR_NOT_FOUND;
    }

    err = cap_cli_capture_run_locked(command_line, &stdout_text, &run_err, &cmd_ret);
    xSemaphoreGive(s_cli.mutex);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to execute CLI command");
        free(command_line);
        return err;
    }

    status = (run_err == ESP_OK && cmd_ret == 0) ? "ok" : "error";
    captured_output = (stdout_text && stdout_text[0]) ? stdout_text : "(none)";
    captured_len = strlen(captured_output);
    keep_len = captured_len;
    if (s_cli.max_output_bytes > 0 && keep_len > s_cli.max_output_bytes) {
        keep_len = claw_utils_utf8_prefix_len(captured_output, s_cli.max_output_bytes);
        truncated_suffix = "\n[truncated]";
    }

    snprintf(output,
             output_size,
             "command: %s\nstatus: %s\nesp_err: %s\ncmd_ret: %d\noutput:\n%.*s%s",
             command_line,
             status,
             esp_err_to_name(run_err),
             cmd_ret,
             (int)keep_len,
             captured_output,
             truncated_suffix);

    ESP_LOGI(TAG, "command=%s esp_err=%s cmd_ret=%d", command_line, esp_err_to_name(run_err), cmd_ret);
    free(stdout_text);
    free(command_line);
    return run_err == ESP_OK && cmd_ret == 0 ? ESP_OK : (run_err != ESP_OK ? run_err : ESP_FAIL);
}

static claw_cap_descriptor_t s_cli_descriptors[] = {
    {
        .id = CAP_CLI_NAME,
        .name = CAP_CLI_NAME,
        .family = "system",
        .description = NULL,
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = CAP_CLI_INPUT_SCHEMA_JSON,
        .execute = cap_cli_execute,
    },
};

static const claw_cap_group_t s_cli_group = {
    .group_id = "cap_cli",
    .descriptors = s_cli_descriptors,
    .descriptor_count = sizeof(s_cli_descriptors) / sizeof(s_cli_descriptors[0]),
};

esp_err_t cap_cli_init(const cap_cli_config_t *config)
{
    cap_cli_entry_t *entries = NULL;
    SemaphoreHandle_t mutex = NULL;

    if (!config || config->max_commands == 0 || config->max_output_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cli.initialized) {
        return ESP_OK;
    }

    entries = calloc(config->max_commands, sizeof(*entries));
    if (!entries) {
        return ESP_ERR_NO_MEM;
    }

    mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        free(entries);
        return ESP_ERR_NO_MEM;
    }

    s_cli.initialized = true;
    s_cli.capacity = config->max_commands;
    s_cli.count = 0;
    s_cli.max_output_bytes = config->max_output_bytes;
    s_cli.entries = entries;
    s_cli.mutex = mutex;
    cap_cli_refresh_description_locked();
    s_cli_descriptors[0].description = s_cli.tool_description;
    return ESP_OK;
}

esp_err_t cap_cli_register_command(const cap_cli_command_t *command)
{
    cap_cli_entry_t entry = {0};
    int index;

    if (!s_cli.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!command || !command->command_name || !command->command_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    entry.command_name = cap_cli_strdup(command->command_name);
    entry.description = cap_cli_strdup(command->description ? command->description : "");
    entry.usage_hint = cap_cli_strdup(command->usage_hint ? command->usage_hint : "");
    if (!entry.command_name || !entry.description || !entry.usage_hint) {
        cap_cli_free_entry(&entry);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_cli.mutex, portMAX_DELAY);
    index = cap_cli_find_command_locked(command->command_name);
    if (index >= 0) {
        cap_cli_free_entry(&s_cli.entries[index]);
        s_cli.entries[(size_t)index] = entry;
        cap_cli_refresh_description_locked();
        xSemaphoreGive(s_cli.mutex);
        return ESP_OK;
    }

    if (s_cli.count >= s_cli.capacity) {
        xSemaphoreGive(s_cli.mutex);
        cap_cli_free_entry(&entry);
        return ESP_ERR_NO_MEM;
    }

    s_cli.entries[s_cli.count++] = entry;
    cap_cli_refresh_description_locked();
    xSemaphoreGive(s_cli.mutex);
    return ESP_OK;
}

esp_err_t cap_cli_register_group(void)
{
    if (!s_cli.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (claw_cap_group_exists(s_cli_group.group_id)) {
        return ESP_OK;
    }

    s_cli_descriptors[0].description = s_cli.tool_description;
    return claw_cap_register_group(&s_cli_group);
}
