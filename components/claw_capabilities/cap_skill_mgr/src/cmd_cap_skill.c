/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_skill.h"

#include <stdio.h>

#include "argtable3/argtable3.h"
#include "claw_skill.h"
#include "esp_console.h"

static struct {
    struct arg_lit *reload;
    struct arg_end *end;
} skill_args;

typedef struct {
    size_t count;
} skill_list_ctx_t;

static esp_err_t print_skill(const claw_skill_catalog_entry_t *entry, void *user_ctx)
{
    skill_list_ctx_t *ctx = user_ctx;
    const char *mode = entry->manage_mode == CLAW_SKILL_MANAGE_MODE_RUNTIME ? "runtime" : "readonly";

    printf("%s\t%s\t%s\n", entry->id, mode, entry->skill_dir);
    ctx->count++;
    return ESP_OK;
}

static int skill_func(int argc, char **argv)
{
    skill_list_ctx_t ctx = {0};
    esp_err_t err;
    int nerrors = arg_parse(argc, argv, (void **)&skill_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, skill_args.end, argv[0]);
        return 1;
    }
    if (skill_args.reload->count) {
        err = claw_skill_reload_registry();
        if (err != ESP_OK) {
            printf("skill reload failed: %s\n", esp_err_to_name(err));
            return 1;
        }
    }
    err = claw_skill_foreach_catalog_entry(print_skill, &ctx);
    if (err != ESP_OK) {
        printf("skill list failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("(%u skills)\n", (unsigned)ctx.count);
    return 0;
}

void register_cap_skill(void)
{
    skill_args.reload = arg_lit0("r", "reload", "Reload the registry before listing skills");
    skill_args.end = arg_end(2);

    const esp_console_cmd_t skill_cmd = {
        .command = "skill",
        .help = "List registered skills. Use --reload to rescan skill directories first.",
        .func = skill_func,
        .argtable = &skill_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&skill_cmd));
}
