/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_pet_buddy.h"

#include <stdbool.h>
#include <stdio.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "pet_buddy.h"

esp_err_t cap_pet_buddy_set_action(const char *action, bool keep_pos)
{
    if (action == NULL || action[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return pet_buddy_action(action, keep_pos);
}

static void cap_pet_buddy_write_status(char *output,
                                       size_t output_size,
                                       bool ok,
                                       const char *status)
{
    if (output == NULL || output_size == 0) {
        return;
    }
    (void)snprintf(output, output_size,
                   "{\"ok\":%s,\"status\":\"%s\"}",
                   ok ? "true" : "false",
                   status != NULL ? status : "unknown");
}

static esp_err_t cap_pet_buddy_action_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = input_json != NULL ? cJSON_Parse(input_json) : NULL;
    const char *action = NULL;
    bool keep_pos = true;
    esp_err_t ret;

    (void)ctx;
    if (root == NULL) {
        cap_pet_buddy_write_status(output, output_size, false, "invalid_json");
        return ESP_ERR_INVALID_ARG;
    }

    action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
    if (action == NULL || action[0] == '\0') {
        cJSON_Delete(root);
        cap_pet_buddy_write_status(output, output_size, false, "missing_action");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *keep_pos_item = cJSON_GetObjectItem(root, "keep_pos");
    if (cJSON_IsBool(keep_pos_item)) {
        keep_pos = cJSON_IsTrue(keep_pos_item);
    }

    ret = cap_pet_buddy_set_action(action, keep_pos);
    cJSON_Delete(root);
    cap_pet_buddy_write_status(output, output_size, ret == ESP_OK,
                               ret == ESP_OK ? "pet_action_set" : "error");
    return ret;
}

static const claw_cap_descriptor_t s_pet_buddy_descriptors[] = {
    {
        .id = "pet_buddy_action",
        .name = "pet_buddy_action",
        .family = "pet_buddy",
        .description = "Set the current Pet Buddy action. Applies to the active attached scene pet, or opens the empty pet module first when no scene is active.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"Pet action, for example idle, jumping, running-left, running-right, waving, failed, waiting, review.\"},\"keep_pos\":{\"type\":\"boolean\",\"description\":\"Keep current pet position when switching action. Defaults to true.\"}},\"required\":[\"action\"]}",
        .execute = cap_pet_buddy_action_execute,
    },
};

static const claw_cap_group_t s_pet_buddy_group = {
    .group_id = "cap_pet_buddy",
    .plugin_name = "pet_buddy",
    .version = "1",
    .descriptors = s_pet_buddy_descriptors,
    .descriptor_count = sizeof(s_pet_buddy_descriptors) / sizeof(s_pet_buddy_descriptors[0]),
};

esp_err_t cap_pet_buddy_register_group(void)
{
    if (claw_cap_group_exists(s_pet_buddy_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_pet_buddy_group);
}
