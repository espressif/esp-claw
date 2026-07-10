/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_TASK_STACK_INTERNAL_ONLY = 0,
    CLAW_TASK_STACK_PREFER_PSRAM,
    CLAW_TASK_STACK_PSRAM_ONLY,
} claw_task_stack_policy_t;

typedef struct {
    const char *name;
    uint32_t stack_size;
    UBaseType_t priority;
    BaseType_t core_id;
    claw_task_stack_policy_t stack_policy;
} claw_task_config_t;

/**
 * @brief Creates a task using the provided config, optionally overridden by the internal task config table
 *
 * This function is similar to xTaskCreatePinnedToCoreWithCaps(), except that it
 * first resolves the caller-provided config against the internal override table.
 * If an entry with the same task name exists in the override table, that entry
 * is used as the effective task config. Otherwise, the caller-provided config is
 * used as-is.
 *
 * The selected stack policy controls whether the task stack is allocated from
 * internal RAM or PSRAM. However, the selected memory capabilities will NOT
 * apply to the task's TCB as a TCB must always be in internal RAM.
 *
 * @param config Pointer to the caller-provided task config
 * @param task_func Pointer to the task entry function
 * @param arg Pointer that will be used as the parameter for the task being created
 * @param task_handle Used to pass back a handle by which the created task can be referenced
 * @return pdPASS if the task was successfully created and added to a ready list,
 * otherwise an error code defined in the file projdefs.h
 */
BaseType_t claw_task_create(const claw_task_config_t *config,
                            TaskFunction_t task_func,
                            void *arg,
                            TaskHandle_t *task_handle);

/**
 * @brief Deletes a task created through xTaskCreatePinnedToCoreWithCaps()
 *
 * This function is similar to vTaskDeleteWithCaps() and should be used to
 * delete tasks whose stacks were allocated with explicit memory capabilities.
 *
 * @param task_handle Handle of the task to delete, or NULL to delete the calling task
 */
void claw_task_delete(TaskHandle_t task_handle);

#ifdef __cplusplus
}
#endif
