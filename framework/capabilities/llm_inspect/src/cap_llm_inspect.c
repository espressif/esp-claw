/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_llm_inspect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"

static esp_err_t cap_llm_inspect_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output,
                                         size_t output_size)
{
    cJSON *root = NULL;
    cJSON *path_json = NULL;
    cJSON *prompt_json = NULL;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    path_json = cJSON_GetObjectItem(root, "path");
    prompt_json = cJSON_GetObjectItem(root, "prompt");
    if (!cJSON_IsString(path_json) || !path_json->valuestring[0] ||
            !cJSON_IsString(prompt_json) || !prompt_json->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path and prompt are required");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);
    snprintf(output, output_size, "Error: image inspection is not supported by claw_agent C ABI");
    return ESP_ERR_NOT_SUPPORTED;
}

static const claw_cap_descriptor_t s_llm_inspect_descriptors[] = {
    {
        .id = "inspect_image",
        .name = "inspect_image",
        .family = "system",
        .description =
        "Analyze a local image from an absolute path. Confirm the path first, then provide a prompt describing what to inspect.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = 0,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"prompt\":{\"type\":\"string\"}},\"required\":[\"path\",\"prompt\"]}",
        .execute = cap_llm_inspect_execute,
    },
};

static const claw_cap_group_t s_llm_inspect_group = {
    .group_id = "cap_llm_inspect",
    .descriptors = s_llm_inspect_descriptors,
    .descriptor_count = sizeof(s_llm_inspect_descriptors) / sizeof(s_llm_inspect_descriptors[0]),
};

esp_err_t cap_llm_inspect_register_group(void)
{
    if (claw_cap_group_exists(s_llm_inspect_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_llm_inspect_group);
}
