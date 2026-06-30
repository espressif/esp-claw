/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

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

BaseType_t claw_task_create(const claw_task_config_t *config,
                            TaskFunction_t task_func,
                            void *arg,
                            TaskHandle_t *task_handle);

void claw_task_delete(TaskHandle_t task_handle);

#ifdef __cplusplus
}
#endif
