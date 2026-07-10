/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_router_mgr.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "cJSON.h"
#include "claw_cap.h"
#include "claw_event_publisher.h"
#include "claw_event_router.h"
#include "esp_console.h"

#define CMD_CAP_ROUTER_MGR_OUTPUT_SIZE 4096

static struct {
    struct arg_lit *reload;
    struct arg_lit *rules;
    struct arg_str *rule;
    struct arg_str *add_rule_json;
    struct arg_str *update_rule_json;
    struct arg_str *delete_rule;
    struct arg_lit *last;
    struct arg_lit *emit_message;
    struct arg_lit *emit_trigger;
    struct arg_str *source_cap;
    struct arg_str *channel;
    struct arg_str *chat_id;
    struct arg_str *text;
    struct arg_str *event_type;
    struct arg_str *event_key;
    struct arg_str *payload_json;
    struct arg_end *end;
} router_args;

static char *event_router_join_args_from(int argc, char **argv, int start_index)
{
    char *joined = NULL;
    size_t joined_len = 0;

    if (!argv || start_index >= argc) {
        return NULL;
    }

    for (int i = start_index; i < argc; i++) {
        joined_len += strlen(argv[i]) + 1;
    }

    joined = calloc(1, joined_len + 1);
    if (!joined) {
        return NULL;
    }

    for (int i = start_index; i < argc; i++) {
        if (i > start_index) {
            strcat(joined, " ");
        }
        strcat(joined, argv[i]);
    }

    return joined;
}

static char *event_router_dup_unwrapped_json(const char *value)
{
    const char *start = value;
    const char *end = NULL;
    size_t len;
    char *copy = NULL;

    if (!value) {
        return NULL;
    }

    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }

    if ((end - start) >= 2 &&
        ((start[0] == '\'' && end[-1] == '\'') || (start[0] == '"' && end[-1] == '"'))) {
        start++;
        end--;
    }

    len = (size_t)(end - start);
    copy = calloc(1, len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, start, len);
    return copy;
}

static esp_err_t event_router_prepare_argv(int argc,
                                           char **argv,
                                           int *out_argc,
                                           char ***out_argv,
                                           char **out_joined_value)
{
    int join_index = -1;
    char **normalized_argv = NULL;

    if (!argv || !out_argc || !out_argv || !out_joined_value) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_argc = argc;
    *out_argv = argv;
    *out_joined_value = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--add-rule-json") == 0 ||
            strcmp(argv[i], "--update-rule-json") == 0 ||
            strcmp(argv[i], "--text") == 0 ||
            strcmp(argv[i], "--payload-json") == 0) {
            join_index = i;
        }
    }

    if (join_index < 0 || (join_index + 1) >= argc) {
        return ESP_OK;
    }

    *out_joined_value = event_router_join_args_from(argc, argv, join_index + 1);
    if (!*out_joined_value) {
        return ESP_ERR_NO_MEM;
    }

    normalized_argv = calloc((size_t)join_index + 2, sizeof(char *));
    if (!normalized_argv) {
        free(*out_joined_value);
        *out_joined_value = NULL;
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i <= join_index; i++) {
        normalized_argv[i] = argv[i];
    }
    normalized_argv[join_index + 1] = *out_joined_value;

    *out_argc = join_index + 2;
    *out_argv = normalized_argv;
    return ESP_OK;
}

static int call_router_mgr_cap(const char *cap_name, const char *input_json)
{
    char *output = NULL;
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_CONSOLE,
        .session_id = "default",
    };
    esp_err_t err;
    int rc = 1;

    output = calloc(1, CMD_CAP_ROUTER_MGR_OUTPUT_SIZE);
    if (!output) {
        printf("Out of memory\n");
        return 1;
    }

    err = claw_cap_call(cap_name, input_json, &ctx, output, CMD_CAP_ROUTER_MGR_OUTPUT_SIZE);
    if (err == ESP_OK) {
        printf("%s\n", output);
        rc = 0;
        goto cleanup;
    }

    printf("%s\n", output[0] ? output : esp_err_to_name(err));
    rc = 1;

cleanup:
    free(output);
    return rc;
}

static char *build_id_json(const char *id)
{
    cJSON *root = NULL;
    char *rendered = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "id", id);
    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return rendered;
}

static char *build_rule_json_input(const char *rule_json)
{
    cJSON *root = NULL;
    char *rendered = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "rule_json", rule_json);
    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return rendered;
}

static int event_router_func(int argc, char **argv)
{
    claw_event_router_result_t result = {0};
    char *input_json = NULL;
    char *joined_value = NULL;
    char **parse_argv = argv;
    int parse_argc = argc;
    esp_err_t err;
    int nerrors;
    int operation_count;
    int rc;

    err = event_router_prepare_argv(argc, argv, &parse_argc, &parse_argv, &joined_value);
    if (err != ESP_OK) {
        printf("Out of memory\n");
        return 1;
    }

    nerrors = arg_parse(parse_argc, parse_argv, (void **)&router_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, router_args.end, parse_argv[0]);
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_value);
        return 1;
    }

    operation_count = router_args.reload->count + router_args.rules->count + router_args.rule->count +
                      router_args.add_rule_json->count + router_args.update_rule_json->count +
                      router_args.delete_rule->count +
                      router_args.last->count + router_args.emit_message->count +
                      router_args.emit_trigger->count;
    if (operation_count != 1) {
        printf("Exactly one operation must be specified\n");
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_value);
        return 1;
    }

    if (router_args.rules->count) {
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_value);
        return call_router_mgr_cap("list_router_rules", "{}");
    }

    if (router_args.rule->count) {
        input_json = build_id_json(router_args.rule->sval[0]);
        if (!input_json) {
            printf("Out of memory\n");
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_value);
            return 1;
        }

        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_value);
        rc = call_router_mgr_cap("get_router_rule", input_json);
        free(input_json);
        return rc;
    }

    if (router_args.add_rule_json->count) {
        input_json = build_rule_json_input(router_args.add_rule_json->sval[0]);
        if (!input_json) {
            printf("Out of memory\n");
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_value);
            return 1;
        }

        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_value);
        rc = call_router_mgr_cap("add_router_rule", input_json);
        free(input_json);
        return rc;
    }

    if (router_args.update_rule_json->count) {
        input_json = build_rule_json_input(router_args.update_rule_json->sval[0]);
        if (!input_json) {
            printf("Out of memory\n");
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_value);
            return 1;
        }

        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_value);
        rc = call_router_mgr_cap("update_router_rule", input_json);
        free(input_json);
        return rc;
    }

    if (router_args.delete_rule->count) {
        input_json = build_id_json(router_args.delete_rule->sval[0]);
        if (!input_json) {
            printf("Out of memory\n");
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_value);
            return 1;
        }

        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_value);
        rc = call_router_mgr_cap("delete_router_rule", input_json);
        free(input_json);
        return rc;
    }

    if (router_args.reload->count) {
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_value);
        return call_router_mgr_cap("reload_router_rules", "{}");
    }

    if (router_args.last->count) {
        err = claw_event_router_get_last_result(&result);
        if (err != ESP_OK) {
            printf("event_router last failed: %s\n", esp_err_to_name(err));
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_value);
            return 1;
        }

        printf("matched=%s matched_rules=%d action_count=%d failed_actions=%d route=%d handled_at_ms=%" PRId64 "\n",
               result.matched ? "true" : "false",
               result.matched_rules,
               result.action_count,
               result.failed_actions,
               (int)result.route,
               result.handled_at_ms);
        printf("first_rule_id=%s\n", result.first_rule_id[0] ? result.first_rule_id : "-");
        printf("ack=%s\n", result.ack[0] ? result.ack : "-");
        printf("last_error=%s\n", esp_err_to_name(result.last_error));
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_value);
        return 0;
    }

    if (router_args.emit_message->count) {
        if (!router_args.source_cap->count || !router_args.channel->count ||
                !router_args.chat_id->count || !router_args.text->count) {
            printf("'--emit-message' requires '--source-cap', '--channel', '--chat-id', and '--text'\n");
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_value);
            return 1;
        }

        err = claw_event_router_publish_message(router_args.source_cap->sval[0],
                                                router_args.channel->sval[0],
                                                router_args.chat_id->sval[0],
                                                router_args.text->sval[0],
                                                "console",
                                                "cli-msg");
        if (err != ESP_OK) {
            printf("event_router emit-message failed: %s\n", esp_err_to_name(err));
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_value);
            return 1;
        }

        printf("message event published via %s to %s:%s\n",
               router_args.source_cap->sval[0],
               router_args.channel->sval[0],
               router_args.chat_id->sval[0]);
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_value);
        return 0;
    }

    if (!router_args.source_cap->count || !router_args.event_type->count ||
            !router_args.event_key->count || !router_args.payload_json->count) {
        printf("'--emit-trigger' requires '--source-cap', '--event-type', '--event-key', and '--payload-json'\n");
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_value);
        return 1;
    }

    {
        char *payload_json = event_router_dup_unwrapped_json(router_args.payload_json->sval[0]);
        cJSON *json = payload_json ? cJSON_Parse(payload_json) : NULL;

        if (!json || !cJSON_IsObject(json)) {
            cJSON_Delete(json);
            free(payload_json);
            printf("'--payload-json' must be a JSON object\n");
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_value);
            return 1;
        }
        cJSON_Delete(json);

        err = claw_event_router_publish_trigger(router_args.source_cap->sval[0],
                                                router_args.event_type->sval[0],
                                                router_args.event_key->sval[0],
                                                payload_json);
        free(payload_json);
    }

    if (err != ESP_OK) {
        printf("event_router emit-trigger failed: %s\n", esp_err_to_name(err));
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_value);
        return 1;
    }

    printf("trigger event published via %s type=%s key=%s\n",
           router_args.source_cap->sval[0],
           router_args.event_type->sval[0],
           router_args.event_key->sval[0]);
    free(parse_argv == argv ? NULL : parse_argv);
    free(joined_value);
    return 0;
}

void register_cap_router_mgr(void)
{
    router_args.reload = arg_lit0(NULL, "reload", "Reload automation rules from disk");
    router_args.rules = arg_lit0(NULL, "rules", "List all automation rules");
    router_args.rule = arg_str0(NULL, "rule", "<id>", "Show one automation rule");
    router_args.add_rule_json = arg_str0(NULL, "add-rule-json", "<json>", "Add one automation rule");
    router_args.update_rule_json = arg_str0(NULL, "update-rule-json", "<json>", "Replace one automation rule by id");
    router_args.delete_rule = arg_str0(NULL, "delete-rule", "<id>", "Delete one automation rule");
    router_args.last = arg_lit0(NULL, "last", "Show the last event router result");
    router_args.emit_message = arg_lit0(NULL, "emit-message", "Publish a message event");
    router_args.emit_trigger = arg_lit0(NULL, "emit-trigger", "Publish a trigger event");
    router_args.source_cap = arg_str0(NULL, "source-cap", "<cap>", "Source capability name");
    router_args.channel = arg_str0(NULL, "channel", "<channel>", "Source channel name");
    router_args.chat_id = arg_str0(NULL, "chat-id", "<chat_id>", "Chat id");
    router_args.text = arg_str0(NULL, "text", "<text>", "Message text");
    router_args.event_type = arg_str0(NULL, "event-type", "<type>", "Trigger event type");
    router_args.event_key = arg_str0(NULL, "event-key", "<key>", "Trigger event key");
    router_args.payload_json = arg_str0(NULL, "payload-json", "<json>", "Trigger payload JSON object");
    router_args.end = arg_end(15);

    const esp_console_cmd_t router_cmd = {
        .command = "event_router",
        .help = "Event router operations.\n"
        "Examples:\n"
        " event_router --rules\n"
        " event_router --rule sample-id\n"
        " event_router --add-rule-json '{\"id\":\"sample\",\"match\":{\"event_type\":\"message\"},\"actions\":[{\"type\":\"drop\"}]}'\n"
        " event_router --update-rule-json '{...}'\n"
        " event_router --delete-rule sample-id\n"
        " event_router --reload\n"
        " event_router --emit-message --source-cap qq_gateway --channel qq --chat-id 123 --text hello\n",
        .func = event_router_func,
        .argtable = &router_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&router_cmd));
}
