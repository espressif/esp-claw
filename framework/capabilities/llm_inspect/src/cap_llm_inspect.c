/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_llm_inspect.h"

#include <stdio.h>
#include <string.h>

#include "claw_cabi_esp.h"

static esp_err_t cap_llm_inspect_execute_impl(const char *input_json,
                                              char *output,
                                              size_t output_size)
{
    (void)input_json;
    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(output,
             output_size,
             "Error: inspect_image is disabled during claw-cabi migration; TODO: reconnect through the Rust agent media API");
    return ESP_ERR_NOT_SUPPORTED;
}

CLAW_CABI_ESP_TOOL_CALLBACK(cap_llm_inspect_execute, cap_llm_inspect_execute_impl)

static const claw_capability_t s_llm_inspect_descriptors[] = {
    CLAW_CABI_ESP_TOOL_DESCRIPTOR(
        "inspect_image",
        "Analyze a local image from an absolute path. Confirm the path first, then provide a prompt describing what to inspect.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"prompt\":{\"type\":\"string\"}},\"required\":[\"path\",\"prompt\"]}",
        cap_llm_inspect_execute),
};

static const claw_capability_group_t s_llm_inspect_group = {
    .id = "cap_llm_inspect",
    .members = s_llm_inspect_descriptors,
    .member_count = sizeof(s_llm_inspect_descriptors) / sizeof(s_llm_inspect_descriptors[0]),
};

esp_err_t cap_llm_inspect_register_group(claw_capability_registry_t *registry)
{
    if (!registry) {
        return ESP_ERR_INVALID_ARG;
    }

    return claw_cabi_register_group_esp(registry, &s_llm_inspect_group);
}
