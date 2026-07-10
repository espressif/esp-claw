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
#include "claw_core_llm.h"

static const char *CAP_LLM_INSPECT_SYSTEM_PROMPT =
    "You analyze local image files for the ESP32 claw. "
    "Describe visible content plainly and briefly. "
    "If the image is unclear, say what is uncertain instead of guessing.";

static esp_err_t cap_llm_inspect_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output,
                                         size_t output_size)
{
    claw_media_asset_t asset = {0};
    claw_llm_media_request_t request = {0};
    cJSON *root = NULL;
    cJSON *path_json = NULL;
    cJSON *prompt_json = NULL;
    char *analysis = NULL;
    char *error_message = NULL;
    esp_err_t err;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx || !ctx->core) {
        snprintf(output, output_size, "Error: claw_core is not ready");
        return ESP_ERR_INVALID_STATE;
    }

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

    asset.kind = CLAW_MEDIA_ASSET_KIND_LOCAL_PATH;
    asset.path = path_json->valuestring;
    request.system_prompt = CAP_LLM_INSPECT_SYSTEM_PROMPT;
    request.user_prompt = prompt_json->valuestring;
    request.media = &asset;
    request.media_count = 1;
    err = claw_core_llm_infer_media(ctx->core, &request, &analysis, &error_message);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output,
                 output_size,
                 "Error: image analysis failed (%s)%s%s",
                 esp_err_to_name(err),
                 error_message ? ": " : "",
                 error_message ? error_message : "");
        free(error_message);
        return err;
    }

    snprintf(output, output_size, "%s", analysis ? analysis : "");
    free(analysis);
    free(error_message);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_llm_inspect_descriptors[] = {
    {
        .id = "inspect_image",
        .name = "inspect_image",
        .family = "system",
        .description =
        "Analyze a local image from an absolute path. Confirm the path first, then provide a prompt describing what to inspect.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
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
