/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_llm_inspect.h"

#include <stdio.h>
#include <stdlib.h>

#include "argtable3/argtable3.h"
#include "cJSON.h"
#include "claw_cabi.h"
#include "claw_cabi_esp.h"
#include "esp_console.h"

static struct {
    struct arg_str *path;
    struct arg_str *prompt;
    struct arg_end *end;
} inspect_args;

static claw_capability_registry_t *s_registry;

static int llm_inspect_func(int argc, char **argv)
{
    cJSON *root = NULL;
    char *input_json = NULL;
    char *output = NULL;
    size_t output_length = 0;
    bool output_success = false;
    esp_err_t err;
    int nerrors = arg_parse(argc, argv, (void **)&inspect_args);

    if (!s_registry) {
        printf("capability registry is not configured\n");
        return 1;
    }

    if (nerrors != 0) {
        arg_print_errors(stderr, inspect_args.end, argv[0]);
        return 1;
    }

    if (!inspect_args.path->count || !inspect_args.prompt->count) {
        printf("'--path' and '--prompt' are required\n");
        return 1;
    }

    root = cJSON_CreateObject();
    if (!root) {
        printf("Out of memory\n");
        return 1;
    }

    cJSON_AddStringToObject(root, "path", inspect_args.path->sval[0]);
    cJSON_AddStringToObject(root, "prompt", inspect_args.prompt->sval[0]);
    input_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!input_json) {
        printf("Out of memory\n");
        return 1;
    }

    output = calloc(1, 2048);
    if (!output) {
        free(input_json);
        printf("Out of memory\n");
        return 1;
    }

    err = claw_cabi_result_to_esp(claw_capability_invoke(s_registry, "inspect_image", input_json,
                                                         output, 2048, &output_length,
                                                         &output_success));
    if (err != ESP_OK) {
        printf("%s\n", output[0] ? output : esp_err_to_name(err));
    } else {
        printf("%s\n", output);
    }

    free(output);
    free(input_json);
    return (err == ESP_OK && output_success) ? 0 : 1;
}

void register_cap_llm_inspect(claw_capability_registry_t *registry)
{
    s_registry = registry;

    inspect_args.path = arg_str1("p", "path", "<path>", "Absolute local image path");
    inspect_args.prompt = arg_str1(NULL, "prompt", "<prompt>", "Inspection prompt");
    inspect_args.end = arg_end(4);

    const esp_console_cmd_t inspect_cmd = {
        .command = "llm_inspect",
        .help = "Inspect a local image with the configured LLM.\n"
        "Example:\n"
        " llm_inspect --path <storage_root>/inbox/pic.jpg --prompt \"Describe the screen contents\"\n",
        .func = llm_inspect_func,
        .argtable = &inspect_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&inspect_cmd));
}
