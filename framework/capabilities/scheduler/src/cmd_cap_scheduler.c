/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "cap_scheduler.h"
#include "cap_scheduler_internal.h"
#include "esp_console.h"

static struct {
    struct arg_lit *list;
    struct arg_lit *reload;
    struct arg_lit *add;
    struct arg_lit *update;
    struct arg_lit *remove;
    struct arg_lit *enable;
    struct arg_lit *disable;
    struct arg_lit *pause;
    struct arg_lit *resume;
    struct arg_lit *trigger;
    struct arg_str *id;
    struct arg_str *json;
    struct arg_end *end;
} scheduler_args;

static char *scheduler_join_args_from(int argc, char **argv, int start_index)
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

static esp_err_t scheduler_prepare_argv(int argc,
                                        char **argv,
                                        int *out_argc,
                                        char ***out_argv,
                                        char **out_joined_json)
{
    bool has_json_op = false;
    int json_index = -1;
    char **normalized_argv = NULL;

    if (!argv || !out_argc || !out_argv || !out_joined_json) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_argc = argc;
    *out_argv = argv;
    *out_joined_json = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--add") == 0 || strcmp(argv[i], "--update") == 0) {
            has_json_op = true;
        } else if (strcmp(argv[i], "--json") == 0 || strcmp(argv[i], "-j") == 0) {
            json_index = i;
        }
    }

    if (!has_json_op || json_index < 0 || (json_index + 1) >= argc) {
        return ESP_OK;
    }

    *out_joined_json = scheduler_join_args_from(argc, argv, json_index + 1);
    if (!*out_joined_json) {
        return ESP_ERR_NO_MEM;
    }

    normalized_argv = calloc((size_t)json_index + 2, sizeof(char *));
    if (!normalized_argv) {
        free(*out_joined_json);
        *out_joined_json = NULL;
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i <= json_index; i++) {
        normalized_argv[i] = argv[i];
    }
    normalized_argv[json_index + 1] = *out_joined_json;

    *out_argc = json_index + 2;
    *out_argv = normalized_argv;
    return ESP_OK;
}

static int scheduler_func(int argc, char **argv)
{
    cap_scheduler_item_t item = {0};
    char *output = NULL;
    char *joined_json = NULL;
    char **parse_argv = argv;
    int parse_argc = argc;
    esp_err_t err;
    int operation_count;
    int nerrors;

    err = scheduler_prepare_argv(argc, argv, &parse_argc, &parse_argv, &joined_json);
    if (err != ESP_OK) {
        printf("Out of memory\n");
        return 1;
    }

    nerrors = arg_parse(parse_argc, parse_argv, (void **)&scheduler_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, scheduler_args.end, parse_argv[0]);
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_json);
        return 1;
    }

    operation_count = scheduler_args.list->count + scheduler_args.reload->count +
                      scheduler_args.add->count +
                      scheduler_args.update->count + scheduler_args.remove->count +
                      scheduler_args.enable->count + scheduler_args.disable->count +
                      scheduler_args.pause->count + scheduler_args.resume->count +
                      scheduler_args.trigger->count;
    if (operation_count != 1) {
        printf("Exactly one operation must be specified\n");
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_json);
        return 1;
    }

    if (scheduler_args.reload->count) {
        err = cap_scheduler_reload();
        if (err != ESP_OK) {
            printf("scheduler reload failed: %s\n", esp_err_to_name(err));
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_json);
            return 1;
        }
        printf("scheduler definitions reloaded\n");
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_json);
        return 0;
    }

    if (scheduler_args.list->count) {
        output = calloc(1, 8192);
        if (!output) {
            printf("Out of memory\n");
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_json);
            return 1;
        }
        err = cap_scheduler_list_json(output, 8192);
        if (err != ESP_OK) {
            printf("scheduler list failed: %s\n", esp_err_to_name(err));
            free(output);
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_json);
            return 1;
        }
        printf("%s\n", output);
        free(output);
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_json);
        return 0;
    }

    if (scheduler_args.add->count || scheduler_args.update->count) {
        if (!scheduler_args.json->count) {
            printf("'--json' is required for this operation\n");
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_json);
            return 1;
        }

        err = cap_scheduler_parse_item_json_string(scheduler_args.json->sval[0], &item);
        if (err != ESP_OK) {
            printf("scheduler add input invalid: %s\n", esp_err_to_name(err));
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_json);
            return 1;
        }

        if (scheduler_args.add->count) {
            err = cap_scheduler_add(&item);
        } else {
            err = cap_scheduler_update(&item);
        }
        if (err != ESP_OK) {
            printf("scheduler %s failed: %s\n",
                   scheduler_args.add->count ? "add" : "update",
                   esp_err_to_name(err));
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_json);
            return 1;
        }

        output = calloc(1, 2048);
        if (!output) {
            printf("Out of memory\n");
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_json);
            return 1;
        }
        err = cap_scheduler_get_state_json(item.id, output, 2048);
        if (err != ESP_OK) {
            printf("scheduler show failed: %s\n", esp_err_to_name(err));
            free(output);
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_json);
            return 1;
        }
        printf("%s\n", output);
        free(output);
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_json);
        return 0;
    }

    if (!scheduler_args.id->count) {
        printf("'--id' is required for this operation\n");
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_json);
        return 1;
    }

    if (scheduler_args.remove->count) {
        err = cap_scheduler_remove(scheduler_args.id->sval[0]);
        if (err != ESP_OK) {
            printf("scheduler remove failed: %s\n", esp_err_to_name(err));
            free(parse_argv == argv ? NULL : parse_argv);
            free(joined_json);
            return 1;
        }
        printf("{\"ok\":true,\"id\":\"%s\",\"removed\":true}\n", scheduler_args.id->sval[0]);
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_json);
        return 0;
    }

    if (scheduler_args.enable->count) {
        err = cap_scheduler_enable(scheduler_args.id->sval[0], true);
    } else if (scheduler_args.disable->count) {
        err = cap_scheduler_enable(scheduler_args.id->sval[0], false);
    } else if (scheduler_args.pause->count) {
        err = cap_scheduler_pause(scheduler_args.id->sval[0]);
    } else if (scheduler_args.resume->count) {
        err = cap_scheduler_resume(scheduler_args.id->sval[0]);
    } else {
        err = cap_scheduler_trigger_now(scheduler_args.id->sval[0]);
    }

    if (err != ESP_OK) {
        printf("scheduler operation failed: %s\n", esp_err_to_name(err));
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_json);
        return 1;
    }

    output = calloc(1, 2048);
    if (!output) {
        printf("Out of memory\n");
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_json);
        return 1;
    }
    err = cap_scheduler_get_state_json(scheduler_args.id->sval[0], output, 2048);
    if (err != ESP_OK) {
        printf("scheduler show failed: %s\n", esp_err_to_name(err));
        free(output);
        free(parse_argv == argv ? NULL : parse_argv);
        free(joined_json);
        return 1;
    }
    printf("%s\n", output);
    free(output);
    free(parse_argv == argv ? NULL : parse_argv);
    free(joined_json);
    return 0;
}

void register_cap_scheduler(void)
{
    scheduler_args.list = arg_lit0("l", "list", "List scheduler entries");
    scheduler_args.reload = arg_lit0(NULL, "reload", "Reload scheduler definitions from disk");
    scheduler_args.add = arg_lit0(NULL, "add", "Add one scheduler entry from JSON");
    scheduler_args.update = arg_lit0(NULL, "update", "Update one scheduler entry from JSON");
    scheduler_args.remove = arg_lit0(NULL, "remove", "Remove one scheduler entry by id");
    scheduler_args.enable = arg_lit0(NULL, "enable", "Enable one scheduler entry");
    scheduler_args.disable = arg_lit0(NULL, "disable", "Disable one scheduler entry");
    scheduler_args.pause = arg_lit0(NULL, "pause", "Pause one scheduler entry");
    scheduler_args.resume = arg_lit0(NULL, "resume", "Resume one scheduler entry");
    scheduler_args.trigger = arg_lit0(NULL, "trigger", "Trigger one scheduler entry now");
    scheduler_args.id = arg_str0("i", "id", "<id>", "Scheduler id");
    scheduler_args.json = arg_str0("j", "json", "<json>", "Scheduler item JSON");
    scheduler_args.end = arg_end(14);

    const esp_console_cmd_t scheduler_cmd = {
        .command = "scheduler",
        .help = "Scheduler operations.\n"
                "Examples:\n"
                " scheduler --list\n"
                " scheduler --reload\n"
                " scheduler --add --json '{\"id\":\"demo_interval\",\"kind\":\"interval\",\"interval_ms\":60000,\"text\":\"demo\"}'\n"
                " scheduler --update --json '{\"id\":\"demo_interval\",\"kind\":\"interval\",\"interval_ms\":120000,\"text\":\"updated\"}'\n"
                " scheduler --remove --id demo_interval\n"
                " scheduler --enable --id hourly_ping\n"
                " scheduler --pause --id hourly_ping\n"
                " scheduler --trigger --id hourly_ping\n",
        .func = scheduler_func,
        .argtable = &scheduler_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&scheduler_cmd));
}
