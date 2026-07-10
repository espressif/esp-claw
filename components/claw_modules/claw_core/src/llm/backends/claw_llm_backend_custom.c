/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/backends/claw_llm_backend_custom.h"

#include <stdlib.h>
#include <string.h>

#include "llm/backends/claw_llm_backend_anthropic.h"
#include "llm/backends/claw_llm_backend_openai_compatible.h"

typedef struct custom_backend_registration_node {
    claw_llm_custom_backend_registration_t registration;
    struct custom_backend_registration_node *next;
} custom_backend_registration_node_t;

static custom_backend_registration_node_t *s_registrations = NULL;

typedef const claw_llm_backend_registration_t *(*builtin_backend_registration_getter_t)(void);

static const claw_llm_backend_registration_t *find_builtin_backend_registration(const char *id)
{
    static const builtin_backend_registration_getter_t getters[] = {
        claw_llm_backend_openai_compatible_registration,
        claw_llm_backend_anthropic_registration,
    };
    size_t i;

    if (!id || !id[0]) {
        return NULL;
    }

    for (i = 0; i < sizeof(getters) / sizeof(getters[0]); i++) {
        const claw_llm_backend_registration_t *registration = getters[i]();

        if (registration && registration->id && strcmp(registration->id, id) == 0) {
            return registration;
        }
    }

    return NULL;
}

static const claw_llm_backend_registration_t *find_custom_backend_registration(const char *id)
{
    custom_backend_registration_node_t *node = s_registrations;

    if (!id || !id[0]) {
        return NULL;
    }

    while (node) {
        if (strcmp(node->registration.id, id) == 0) {
            return &node->registration;
        }
        node = node->next;
    }

    return NULL;
}

const claw_llm_backend_registration_t *claw_llm_find_backend_registration(const char *id)
{
    const claw_llm_backend_registration_t *registration = find_builtin_backend_registration(id);

    if (registration) {
        return registration;
    }

    return find_custom_backend_registration(id);
}

const claw_llm_backend_vtable_t *claw_llm_find_custom_backend(const char *id)
{
    const claw_llm_backend_registration_t *registration = find_custom_backend_registration(id);

    return registration ? registration->vtable : NULL;
}

esp_err_t claw_llm_register_custom_backend(const claw_llm_custom_backend_registration_t *registration)
{
    custom_backend_registration_node_t *node;

    if (!registration || !registration->id || !registration->vtable) {
        return ESP_ERR_INVALID_ARG;
    }
    if (claw_llm_find_backend_registration(registration->id)) {
        return ESP_ERR_INVALID_STATE;
    }

    node = calloc(1, sizeof(*node));
    if (!node) {
        return ESP_ERR_NO_MEM;
    }

    node->registration = *registration;
    node->next = s_registrations;
    s_registrations = node;
    return ESP_OK;
}
