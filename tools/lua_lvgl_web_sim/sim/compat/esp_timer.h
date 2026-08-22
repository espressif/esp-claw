#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef void (*esp_timer_cb_t)(void *arg);

typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    const char *name;
} esp_timer_create_args_t;

typedef struct sim_esp_timer *esp_timer_handle_t;

int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out_handle);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t handle, uint64_t period_us);
esp_err_t esp_timer_stop(esp_timer_handle_t handle);
esp_err_t esp_timer_delete(esp_timer_handle_t handle);
