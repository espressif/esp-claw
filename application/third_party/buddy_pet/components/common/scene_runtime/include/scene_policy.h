/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "scene_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *idle_action;
    const char *waiting_action;
    const char *active_action;
    const char *positive_action;
    const char *negative_action;
    const char *neutral_action;
} scene_pet_policy_t;

const char *scene_policy_pick_pet_action(const scene_event_t *event,
                                         const scene_pet_policy_t *policy);

#ifdef __cplusplus
}
#endif
