/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_time.h"

#include <stdio.h>
#include <stdlib.h>

#include "argtable3/argtable3.h"
#include "cap_time.h"
#include "esp_console.h"

static struct {
    struct arg_lit *now;
    struct arg_end *end;
} time_args;

static int time_func(int argc, char **argv)
{
    char output[256] = {0};
    esp_err_t err;
    int nerrors = arg_parse(argc, argv, (void **)&time_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, time_args.end, argv[0]);
        return 1;
    }
    if (!time_args.now->count) {
        printf("Specify '--now' to print current local time and sync with SNTP only when needed\n");
        return 1;
    }

    err = cap_time_get_current(output, sizeof(output));
    if (err != ESP_OK) {
        printf("failed to get time: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("%s\n", output);
    return 0;
}

void register_cap_time(void)
{
    time_args.now = arg_lit0(NULL, "now", "Print current local time and sync with SNTP only when needed");
    time_args.end = arg_end(2);

    const esp_console_cmd_t time_cmd = {
        .command = "time",
        .help = "Time operations.\n"
        "Examples:\n"
        " time --now\n",
        .func = time_func,
        .argtable = &time_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&time_cmd));
}
