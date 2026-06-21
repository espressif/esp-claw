/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*scene_manager_close_cb_t)(void *user_data);

typedef struct {
    const char *name;
    scene_manager_close_cb_t close_cb;
    void *user_data;
} scene_manager_scene_t;

esp_err_t scene_manager_acquire(const scene_manager_scene_t *scene);
void scene_manager_release(const char *name);
const char *scene_manager_active_scene(void);

#ifdef __cplusplus
}
#endif
