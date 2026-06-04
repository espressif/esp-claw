/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_session_mgr.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"

static const char *TAG = "cap_session_mgr";

static const char *s_session_mgr_usage =
    "Usage:\n"
    "/session new [name]       Create and switch to a new session\n"
    "/session list             List sessions in this chat\n"
    "/session switch <name>    Switch to an existing session\n"
    "/session delete <name>    Delete a non-current session\n"
    "\n"
    "Examples:\n"
    "/session new\n"
    "/session new project-a\n"
    "/session list\n"
    "/session switch project-a\n"
    "/session delete project-a\n"
    "\n"
    "Session names must be 1-32 characters and may only contain A-Z, a-z, 0-9, _ or -.";

typedef enum {
    CAP_SESSION_MGR_CMD_NEW,
    CAP_SESSION_MGR_CMD_LIST,
    CAP_SESSION_MGR_CMD_SWITCH,
    CAP_SESSION_MGR_CMD_DELETE,
} cap_session_mgr_command_t;

typedef struct {
    cap_session_mgr_command_t command;
    bool has_alias;
    char alias[CLAW_SESSION_MGR_ALIAS_MAX + 1];
} cap_session_mgr_parsed_command_t;

static bool cap_session_mgr_is_ascii_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

static const char *cap_session_mgr_skip_ascii_space(const char *text)
{
    if (!text) {
        return "";
    }
    while (*text && cap_session_mgr_is_ascii_space(*text)) {
        text++;
    }

    return text;
}

static esp_err_t cap_session_mgr_append_output(char *output,
                                               size_t output_size,
                                               size_t *offset,
                                               const char *fmt,
                                               ...)
{
    va_list args;
    int written;

    if (!output || output_size == 0 || !offset || *offset >= output_size) {
        return ESP_ERR_INVALID_ARG;
    }

    va_start(args, fmt);
    written = vsnprintf(output + *offset, output_size - *offset, fmt, args);
    va_end(args);
    if (written < 0) {
        return ESP_FAIL;
    }
    if ((size_t)written >= output_size - *offset) {
        *offset = output_size - 1;
        return ESP_ERR_INVALID_SIZE;
    }
    *offset += (size_t)written;

    return ESP_OK;
}

static esp_err_t cap_session_mgr_read_command_input(const char *input_json,
                                                    char *command,
                                                    size_t command_size)
{
    cJSON *root = NULL;
    cJSON *item = NULL;

    if (!command || command_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    command[0] = '\0';

    if (!input_json || !input_json[0]) {
        return ESP_OK;
    }
    root = cJSON_Parse(input_json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!item || cJSON_IsNull(item)) {
        cJSON_Delete(root);
        return ESP_OK;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(command, item->valuestring, command_size) >= command_size) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);

    return ESP_OK;
}

static esp_err_t cap_session_mgr_read_token(const char **cursor,
                                            char *token,
                                            size_t token_size,
                                            bool *out_has_token)
{
    const char *start = NULL;
    size_t len;

    if (!cursor || !*cursor || !token || token_size == 0 || !out_has_token) {
        return ESP_ERR_INVALID_ARG;
    }
    token[0] = '\0';
    *out_has_token = false;

    start = cap_session_mgr_skip_ascii_space(*cursor);
    if (!start[0]) {
        *cursor = start;
        return ESP_OK;
    }
    *out_has_token = true;
    len = 0;
    while (start[len] && !cap_session_mgr_is_ascii_space(start[len])) {
        len++;
    }
    if (len >= token_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(token, start, len);
    token[len] = '\0';
    *cursor = start + len;

    return ESP_OK;
}

static esp_err_t cap_session_mgr_parse_session_command(const char *command_text,
                                                       cap_session_mgr_parsed_command_t *parsed)
{
    const char *cursor = command_text;
    char subcommand[8];
    char extra[2];
    bool has_subcommand = false;
    bool has_extra = false;
    esp_err_t err;

    if (!parsed) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(parsed, 0, sizeof(*parsed));

    err = cap_session_mgr_read_token(&cursor, subcommand, sizeof(subcommand), &has_subcommand);
    if (err != ESP_OK || !has_subcommand) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(subcommand, "new") == 0) {
        parsed->command = CAP_SESSION_MGR_CMD_NEW;
    } else if (strcmp(subcommand, "list") == 0) {
        parsed->command = CAP_SESSION_MGR_CMD_LIST;
    } else if (strcmp(subcommand, "switch") == 0) {
        parsed->command = CAP_SESSION_MGR_CMD_SWITCH;
    } else if (strcmp(subcommand, "delete") == 0) {
        parsed->command = CAP_SESSION_MGR_CMD_DELETE;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_session_mgr_read_token(&cursor,
                                     parsed->alias,
                                     sizeof(parsed->alias),
                                     &parsed->has_alias);
    if (err != ESP_OK) {
        return err;
    }
    err = cap_session_mgr_read_token(&cursor, extra, sizeof(extra), &has_extra);
    if (err != ESP_OK || has_extra) {
        return ESP_ERR_INVALID_ARG;
    }

    if (parsed->command == CAP_SESSION_MGR_CMD_LIST && parsed->has_alias) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((parsed->command == CAP_SESSION_MGR_CMD_SWITCH ||
            parsed->command == CAP_SESSION_MGR_CMD_DELETE) &&
            !parsed->has_alias) {
        return ESP_ERR_INVALID_ARG;
    }
    if (parsed->has_alias && !claw_session_mgr_alias_is_valid(parsed->alias)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static void cap_session_mgr_write_message(char *output, size_t output_size, const char *message)
{
    if (output && output_size > 0) {
        snprintf(output, output_size, "%s", message ? message : "");
    }
}

static void cap_session_mgr_write_format(char *output, size_t output_size, const char *fmt, ...)
{
    va_list args;

    if (!output || output_size == 0 || !fmt) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(output, output_size, fmt, args);
    va_end(args);
}

static bool cap_session_mgr_context_has_chat(const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    if (!ctx || !ctx->channel || !ctx->channel[0] || !ctx->chat_id || !ctx->chat_id[0]) {
        cap_session_mgr_write_message(output, output_size, "Session command failed: missing chat context.");
        ESP_LOGW(TAG, "Session command missing channel or chat_id");
        return false;
    }

    return true;
}

static bool cap_session_mgr_context_ready(const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    if (!cap_session_mgr_context_has_chat(ctx, output, output_size)) {
        return false;
    }
    if (!claw_session_mgr_is_configured()) {
        cap_session_mgr_write_message(output, output_size, "Session command failed: session manager is not configured.");
        ESP_LOGW(TAG, "Session manager command called before configuration");
        return false;
    }

    return true;
}

static void cap_session_mgr_write_storage_error(const char *action,
                                                const claw_cap_call_context_t *ctx,
                                                const char *alias,
                                                esp_err_t err,
                                                char *output,
                                                size_t output_size)
{
    ESP_LOGE(TAG,
             "%s chat session failed for %s:%s alias=%s: %s",
             action,
             ctx->channel,
             ctx->chat_id,
             alias ? alias : "",
             esp_err_to_name(err));
    cap_session_mgr_write_message(output,
                                  output_size,
                                  "Session command failed: unable to access session storage.");
}

static esp_err_t cap_session_mgr_write_list_output(const claw_session_mgr_alias_map_t *map,
                                                   char *output,
                                                   size_t output_size)
{
    size_t offset = 0;
    esp_err_t err;

    err = cap_session_mgr_append_output(output, output_size, &offset, "Sessions:");
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < map->session_count; i++) {
        err = cap_session_mgr_append_output(output,
                                            output_size,
                                            &offset,
                                            "\n* %s%s",
                                            map->sessions[i],
                                            strcmp(map->sessions[i], map->current_alias) == 0 ? " (current)" : "");
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_command_execute(cap_session_mgr_command_t command,
                                                 const char *alias,
                                                 bool has_alias,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size)
{
    char result_alias[CLAW_SESSION_MGR_ALIAS_MAX + 1];
    claw_session_mgr_alias_map_t map;
    esp_err_t err;

    if (!cap_session_mgr_context_ready(ctx, output, output_size)) {
        return ESP_OK;
    }

    switch (command) {
    case CAP_SESSION_MGR_CMD_NEW:
        err = claw_session_mgr_new_chat_session(0,
                                                ctx->channel,
                                                ctx->chat_id,
                                                alias,
                                                has_alias,
                                                result_alias,
                                                sizeof(result_alias));
        break;
    case CAP_SESSION_MGR_CMD_LIST:
        err = claw_session_mgr_list_chat_sessions(0, ctx->channel, ctx->chat_id, &map);
        if (err == ESP_OK) {
            err = cap_session_mgr_write_list_output(&map, output, output_size);
        }
        break;
    case CAP_SESSION_MGR_CMD_SWITCH:
        err = claw_session_mgr_switch_chat_session(0,
                                                   ctx->channel,
                                                   ctx->chat_id,
                                                   alias,
                                                   result_alias,
                                                   sizeof(result_alias));
        break;
    case CAP_SESSION_MGR_CMD_DELETE:
        err = claw_session_mgr_delete_chat_session(0,
                                                   ctx->channel,
                                                   ctx->chat_id,
                                                   alias,
                                                   result_alias,
                                                   sizeof(result_alias));
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    if (err == ESP_ERR_INVALID_STATE && command == CAP_SESSION_MGR_CMD_NEW) {
        cap_session_mgr_write_format(output,
                                     output_size,
                                     "Cannot create session \"%s\": a session with that name already exists.",
                                     alias);
        return ESP_OK;
    }
    if (command == CAP_SESSION_MGR_CMD_SWITCH && err == ESP_ERR_NOT_FOUND) {
        cap_session_mgr_write_format(output,
                                     output_size,
                                     "Cannot switch to session \"%s\": no such session. Send /session list to list sessions.",
                                     alias);
        return ESP_OK;
    }
    if (command == CAP_SESSION_MGR_CMD_DELETE && err == ESP_ERR_NOT_FOUND) {
        cap_session_mgr_write_format(output,
                                     output_size,
                                     "Cannot delete session \"%s\": no such session. Send /session list to list sessions.",
                                     alias);
        return ESP_OK;
    }
    if (command == CAP_SESSION_MGR_CMD_DELETE && err == ESP_ERR_INVALID_STATE) {
        cap_session_mgr_write_format(output,
                                     output_size,
                                     "Cannot delete the current session \"%s\". Switch to another session first.",
                                     alias);
        return ESP_OK;
    }
    if (command == CAP_SESSION_MGR_CMD_DELETE && err == ESP_ERR_NOT_SUPPORTED) {
        cap_session_mgr_write_message(output,
                                      output_size,
                                      "Session command failed: session history deletion is unavailable.");
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        cap_session_mgr_write_message(output, output_size, "Session command failed: session manager is not configured.");
        ESP_LOGW(TAG, "Session manager command called before configuration");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        cap_session_mgr_write_storage_error(command == CAP_SESSION_MGR_CMD_NEW ? "Create" :
                                            command == CAP_SESSION_MGR_CMD_LIST ? "List" :
                                            command == CAP_SESSION_MGR_CMD_SWITCH ? "Switch" : "Delete",
                                            ctx,
                                            has_alias ? alias : NULL,
                                            err,
                                            output,
                                            output_size);
        return ESP_OK;
    }

    if (command == CAP_SESSION_MGR_CMD_NEW) {
        ESP_LOGI(TAG, "Created chat session %s:%s alias=%s", ctx->channel, ctx->chat_id, result_alias);
        cap_session_mgr_write_format(output, output_size, "Started a new session: %s", result_alias);
    } else if (command == CAP_SESSION_MGR_CMD_SWITCH) {
        ESP_LOGI(TAG, "Switched chat session %s:%s alias=%s", ctx->channel, ctx->chat_id, result_alias);
        cap_session_mgr_write_format(output, output_size, "Switched to session: %s", result_alias);
    } else if (command == CAP_SESSION_MGR_CMD_DELETE) {
        cap_session_mgr_write_format(output, output_size, "Deleted session: %s", result_alias);
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_session_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size)
{
    char command_text[96];
    cap_session_mgr_parsed_command_t parsed;
    esp_err_t err;

    err = cap_session_mgr_read_command_input(input_json, command_text, sizeof(command_text));
    if (err != ESP_OK ||
            cap_session_mgr_parse_session_command(command_text, &parsed) != ESP_OK) {
        cap_session_mgr_write_message(output, output_size, s_session_mgr_usage);
        return ESP_OK;
    }

    return cap_session_mgr_command_execute(parsed.command,
                                           parsed.has_alias ? parsed.alias : NULL,
                                           parsed.has_alias,
                                           ctx,
                                           output,
                                           output_size);
}

static const claw_cap_descriptor_t s_session_mgr_caps[] = {
    {
        .id = "session_command",
        .name = "session_command",
        .family = "system",
        .description = "Handle consolidated /session chat commands.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}}}",
        .execute = cap_session_mgr_session_execute,
    },
};

static const claw_cap_group_t s_session_mgr_group = {
    .group_id = "cap_session_mgr",
    .plugin_name = "cap_session_mgr",
    .version = "1.0.0",
    .descriptors = s_session_mgr_caps,
    .descriptor_count = sizeof(s_session_mgr_caps) / sizeof(s_session_mgr_caps[0]),
};

esp_err_t cap_session_mgr_register_group(void)
{
    return claw_cap_register_group(&s_session_mgr_group);
}

esp_err_t cap_session_mgr_set_session_root_dir(const char *session_root_dir)
{
    return claw_session_mgr_set_session_root_dir(session_root_dir);
}

esp_err_t cap_session_mgr_set_delete_session_handler(cap_session_mgr_delete_session_fn_t fn,
                                                     void *user_ctx)
{
    return claw_session_mgr_set_delete_session_handler(fn, user_ctx);
}
