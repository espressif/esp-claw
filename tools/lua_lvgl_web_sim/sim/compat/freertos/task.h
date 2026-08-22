#pragma once

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreate(TaskFunction_t task,
                       const char *name,
                       uint32_t stack_depth,
                       void *arg,
                       UBaseType_t priority,
                       TaskHandle_t *out_handle);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait);
void xTaskNotifyGive(TaskHandle_t task);
