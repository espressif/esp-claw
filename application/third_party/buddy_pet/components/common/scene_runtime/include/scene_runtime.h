/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct scene_runtime_t scene_runtime_t;

typedef struct {
    const char *name;
    uint32_t stack_size;
    UBaseType_t priority;
} scene_runtime_task_config_t;

typedef esp_err_t (*scene_runtime_prepare_cb_t)(void *user_data);
typedef void (*scene_runtime_cleanup_cb_t)(void *user_data);
typedef esp_err_t (*scene_runtime_run_cb_t)(void *user_data);
typedef esp_err_t (*scene_runtime_request_close_cb_t)(void *user_data);
typedef esp_err_t (*scene_runtime_task_create_cb_t)(const scene_runtime_task_config_t *config,
                                                    TaskFunction_t task_fn,
                                                    void *task_arg,
                                                    TaskHandle_t *out_task);
typedef void (*scene_runtime_task_delete_cb_t)(void);

typedef struct {
    const char *scene_name;
    const char *task_name;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
    uint32_t close_wait_ms;
    bool repeat_close_request;
    scene_runtime_prepare_cb_t prepare;
    scene_runtime_cleanup_cb_t cleanup;
    scene_runtime_run_cb_t run;
    scene_runtime_request_close_cb_t request_close;
    scene_runtime_task_create_cb_t task_create;
    scene_runtime_task_delete_cb_t task_delete;
    void *user_data;
} scene_runtime_config_t;

struct scene_runtime_t {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool opening;
    bool running;
    bool close_requested;
    scene_runtime_config_t config;
};

esp_err_t scene_runtime_open(scene_runtime_t *runtime,
                             const scene_runtime_config_t *config,
                             bool *already_running);
esp_err_t scene_runtime_close(scene_runtime_t *runtime);
bool scene_runtime_is_running(scene_runtime_t *runtime);

#ifdef __cplusplus
}
#endif
