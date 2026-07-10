/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_task.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

static const char *TAG = "claw_task";

/* Entries in this table override the config passed by the caller. Keep a trailing sentinel so the table can be left effectively empty. */
static const claw_task_config_t s_task_configs[] = {
    {0},
};

static const claw_task_config_t *claw_task_find_override(const char *name)
{
    size_t i;

    if (!name || !name[0]) {
        return NULL;
    }

    for (i = 0; s_task_configs[i].name; i++) {
        if (strcmp(s_task_configs[i].name, name) == 0) {
            return &s_task_configs[i];
        }
    }

    return NULL;
}

static bool claw_task_external_memory_available(void)
{
#if defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
#else
    return false;
#endif
}

static esp_err_t claw_task_resolve_config(const claw_task_config_t *input, claw_task_config_t *out)
{
    const claw_task_config_t *override = NULL;

    if (!input || !input->name || !input->name[0] || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = *input;
    override = claw_task_find_override(input->name);
    if (override) {
        *out = *override;
    }

    return ESP_OK;
}

static UBaseType_t claw_task_memory_caps(claw_task_stack_policy_t policy)
{
    if (policy != CLAW_TASK_STACK_INTERNAL_ONLY && claw_task_external_memory_available()) {
        return MALLOC_CAP_SPIRAM;
    }

    return MALLOC_CAP_INTERNAL;
}

BaseType_t claw_task_create(const claw_task_config_t *config,
                            TaskFunction_t task_func,
                            void *arg,
                            TaskHandle_t *task_handle)
{
    claw_task_config_t resolved = {0};
    UBaseType_t memory_caps;

    if (claw_task_resolve_config(config, &resolved) != ESP_OK || !task_func || resolved.stack_size == 0) {
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }

    memory_caps = claw_task_memory_caps(resolved.stack_policy);
    if (resolved.stack_policy == CLAW_TASK_STACK_PSRAM_ONLY && memory_caps != MALLOC_CAP_SPIRAM) {
        ESP_LOGE(TAG, "task '%s' requires PSRAM stack but PSRAM is unavailable", resolved.name);
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }
    ESP_LOGD(TAG, "Creating task '%s' with stack size %u, priority %u, on core %d, with memory caps %s\n",
           resolved.name,
           (unsigned int)resolved.stack_size,
           (unsigned int)resolved.priority,
           resolved.core_id,
           memory_caps == MALLOC_CAP_SPIRAM ? "SPIRAM" : "INTERNAL");

    return xTaskCreatePinnedToCoreWithCaps(task_func,
                                           resolved.name,
                                           resolved.stack_size,
                                           arg,
                                           resolved.priority,
                                           task_handle,
                                           resolved.core_id,
                                           memory_caps);
}

void claw_task_delete(TaskHandle_t task_handle)
{
    vTaskDeleteWithCaps(task_handle);
}
