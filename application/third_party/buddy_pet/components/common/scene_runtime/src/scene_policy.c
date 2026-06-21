/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "scene_policy.h"

#include <stddef.h>

const char *scene_policy_pick_pet_action(const scene_event_t *event,
                                         const scene_pet_policy_t *policy)
{
    if (event == NULL || policy == NULL) {
        return NULL;
    }

    switch (event->kind) {
    case SCENE_EVENT_KIND_WAITING:
        return policy->waiting_action;
    case SCENE_EVENT_KIND_ACTIVE:
        return policy->active_action;
    case SCENE_EVENT_KIND_POSITIVE:
        return policy->positive_action;
    case SCENE_EVENT_KIND_NEGATIVE:
        return policy->negative_action;
    case SCENE_EVENT_KIND_NEUTRAL:
        return policy->neutral_action;
    case SCENE_EVENT_KIND_IDLE:
    default:
        return policy->idle_action;
    }
}
