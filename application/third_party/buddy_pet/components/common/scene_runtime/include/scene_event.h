/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCENE_EVENT_KIND_IDLE = 0,
    SCENE_EVENT_KIND_WAITING,
    SCENE_EVENT_KIND_ACTIVE,
    SCENE_EVENT_KIND_POSITIVE,
    SCENE_EVENT_KIND_NEGATIVE,
    SCENE_EVENT_KIND_NEUTRAL,
} scene_event_kind_t;

typedef struct {
    scene_event_kind_t kind;
    const char *scene;
    const char *subject;
    uint32_t source_id;
} scene_event_t;

#ifdef __cplusplus
}
#endif
