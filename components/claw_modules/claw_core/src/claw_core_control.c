/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_internal.h"

#include <inttypes.h>

#include "esp_log.h"

static const char *TAG = "claw_core";

void claw_core_control_set_phase(claw_core_state_t *core, claw_core_agent_loop_phase_t phase)
{
    if (!core || !core->inflight_lock) {
        return;
    }

    if (xSemaphoreTake(core->inflight_lock, portMAX_DELAY) == pdTRUE) {
        core->agent_loop_phase = phase;
        xSemaphoreGive(core->inflight_lock);
    }
}

bool claw_core_control_take_user_interrupt_http_abort(claw_core_state_t *core,
                                                      uint32_t request_id)
{
    bool taken = false;

    if (!core || !core->inflight_lock) {
        return false;
    }
    if (xSemaphoreTake(core->inflight_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (core->inflight_request_id == request_id &&
            core->inflight_abort &&
            core->inflight_abort_reason == CLAW_CORE_CONTROL_ABORT_REASON_USER_INTERRUPT) {
        core->inflight_abort = false;
        core->inflight_abort_reason = CLAW_CORE_CONTROL_ABORT_REASON_NONE;
        taken = true;
    }
    xSemaphoreGive(core->inflight_lock);
    return taken;
}

void claw_core_control_clear_user_interrupt_abort(claw_core_state_t *core,
                                                  uint32_t request_id)
{
    if (!core || !core->inflight_lock) {
        return;
    }
    if (xSemaphoreTake(core->inflight_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (core->inflight_request_id == request_id &&
            core->inflight_abort_reason == CLAW_CORE_CONTROL_ABORT_REASON_USER_INTERRUPT) {
        core->inflight_abort = false;
        core->inflight_abort_reason = CLAW_CORE_CONTROL_ABORT_REASON_NONE;
    }
    xSemaphoreGive(core->inflight_lock);
}

esp_err_t claw_core_control_cancel_request(claw_core_state_t *core, uint32_t request_id)
{
    bool armed = false;

    if (!core || !core->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(core->inflight_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (core->inflight_request_id != 0 &&
            (request_id == 0 || core->inflight_request_id == request_id)) {
        core->inflight_abort = true;
        core->inflight_abort_reason = CLAW_CORE_CONTROL_ABORT_REASON_CANCEL;
        armed = true;
        ESP_LOGI(TAG, "Cancel armed for in-flight request=%" PRIu32,
                 core->inflight_request_id);
    }
    xSemaphoreGive(core->inflight_lock);
    return armed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

claw_core_agent_loop_phase_t claw_core_control_get_phase(claw_core_state_t *core)
{
    claw_core_agent_loop_phase_t phase = CLAW_CORE_AGENT_LOOP_PHASE_IDLE;

    if (!core || !core->initialized || !core->inflight_lock) {
        return phase;
    }
    if (xSemaphoreTake(core->inflight_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return phase;
    }
    phase = core->agent_loop_phase;
    xSemaphoreGive(core->inflight_lock);
    return phase;
}
