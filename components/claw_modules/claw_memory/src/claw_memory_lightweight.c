/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_memory_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static esp_err_t claw_memory_long_term_lightweight_collect(const claw_core_request_t *request,
                                                           claw_core_context_t *out_context,
                                                           void *user_ctx)
{
    char *content = NULL;
    esp_err_t err;

    (void)request;
    (void)user_ctx;

    if (!out_context) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_context, 0, sizeof(*out_context));
    err = read_file_dup(s_memory.markdown_path, &content);
    if (err != ESP_OK) {
        return err;
    }
    if (!content[0]) {
        free(content);
        return ESP_ERR_NOT_FOUND;
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = content;
    return ESP_OK;
}

const claw_core_context_provider_t claw_memory_long_term_lightweight_provider = {
    .name = "Long-term Memory",
    .collect = claw_memory_long_term_lightweight_collect,
    .user_ctx = NULL,
};
