/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_web_search.h"

#include <stdio.h>
#include <stdlib.h>

#include "argtable3/argtable3.h"
#include "cJSON.h"
#include "claw_cap.h"
#include "esp_console.h"

static struct {
    struct arg_str *query;
    struct arg_end *end;
} web_search_args;

static int web_search_func(int argc, char **argv)
{
    cJSON *root = NULL;
    char *input_json = NULL;
    char *output = NULL;
    esp_err_t err;
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_CONSOLE,
    };
    int nerrors = arg_parse(argc, argv, (void **)&web_search_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, web_search_args.end, argv[0]);
        return 1;
    }

    if (!web_search_args.query->count) {
        printf("'--query' is required\n");
        return 1;
    }

    root = cJSON_CreateObject();
    if (!root) {
        printf("Out of memory\n");
        return 1;
    }

    cJSON_AddStringToObject(root, "query", web_search_args.query->sval[0]);
    input_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!input_json) {
        printf("Out of memory\n");
        return 1;
    }

    output = calloc(1, 4096);
    if (!output) {
        free(input_json);
        printf("Out of memory\n");
        return 1;
    }

    err = claw_cap_call("web_search", input_json, &ctx, output, 4096);
    if (err != ESP_OK) {
        printf("%s\n", output[0] ? output : esp_err_to_name(err));
    } else {
        printf("%s\n", output);
    }

    free(output);
    free(input_json);
    return err == ESP_OK ? 0 : 1;
}

void register_cap_web_search(void)
{
    web_search_args.query = arg_str1("q", "query", "<query>", "Search query");
    web_search_args.end = arg_end(4);

    const esp_console_cmd_t web_search_cmd = {
        .command = "web_search",
        .help = "Web search operation.\n"
        "Example:\n"
        " web_search --query \"ESP-IDF mDNS example\"\n",
        .func = web_search_func,
        .argtable = &web_search_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&web_search_cmd));
}
