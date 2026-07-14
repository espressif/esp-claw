/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_agent_input.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define CAP_AGENT_MAX_PENDING_INPUTS 32

typedef struct {
    uint32_t session_id;
    uint32_t request_id;
} cap_agent_pending_input_t;

static cap_agent_pending_input_t s_pending_inputs[CAP_AGENT_MAX_PENDING_INPUTS];
static portMUX_TYPE s_pending_inputs_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t cap_agent_input_request_store(uint32_t session_id, uint32_t request_id)
{
    size_t free_slot = CAP_AGENT_MAX_PENDING_INPUTS;

    if (session_id == 0 || request_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_pending_inputs_lock);

    for (size_t i = 0; i < CAP_AGENT_MAX_PENDING_INPUTS; i++) {
        cap_agent_pending_input_t *pending = &s_pending_inputs[i];
        if (pending->session_id == session_id) {
            pending->request_id = request_id;
            portEXIT_CRITICAL(&s_pending_inputs_lock);
            return ESP_OK;
        }
        if (pending->session_id == 0 && free_slot == CAP_AGENT_MAX_PENDING_INPUTS) {
            free_slot = i;
        }
    }

    if (free_slot == CAP_AGENT_MAX_PENDING_INPUTS) {
        portEXIT_CRITICAL(&s_pending_inputs_lock);
        return ESP_ERR_NO_MEM;
    }

    s_pending_inputs[free_slot].session_id = session_id;
    s_pending_inputs[free_slot].request_id = request_id;
    portEXIT_CRITICAL(&s_pending_inputs_lock);
    return ESP_OK;
}

esp_err_t cap_agent_input_request_get(uint32_t session_id, uint32_t *out_request_id)
{
    if (session_id == 0 || !out_request_id) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_pending_inputs_lock);

    for (size_t i = 0; i < CAP_AGENT_MAX_PENDING_INPUTS; i++) {
        const cap_agent_pending_input_t *pending = &s_pending_inputs[i];
        if (pending->session_id == session_id) {
            *out_request_id = pending->request_id;
            portEXIT_CRITICAL(&s_pending_inputs_lock);
            return ESP_OK;
        }
    }

    portEXIT_CRITICAL(&s_pending_inputs_lock);
    return ESP_ERR_NOT_FOUND;
}

void cap_agent_input_request_clear(uint32_t session_id, uint32_t request_id)
{
    if (session_id == 0 || request_id == 0) {
        return;
    }

    portENTER_CRITICAL(&s_pending_inputs_lock);
    for (size_t i = 0; i < CAP_AGENT_MAX_PENDING_INPUTS; i++) {
        cap_agent_pending_input_t *pending = &s_pending_inputs[i];
        if (pending->session_id == session_id && pending->request_id == request_id) {
            pending->session_id = 0;
            pending->request_id = 0;
            break;
        }
    }

    portEXIT_CRITICAL(&s_pending_inputs_lock);
}
