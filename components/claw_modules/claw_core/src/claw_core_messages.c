/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_internal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "claw_core";

static bool claw_core_agent_loop_phase_is_insertable(claw_core_agent_loop_phase_t phase)
{
    return phase != CLAW_CORE_AGENT_LOOP_PHASE_IDLE &&
           phase != CLAW_CORE_AGENT_LOOP_PHASE_FINALIZING;
}

void claw_core_free_request_item(claw_core_request_item_t *item)
{
    if (!item) {
        return;
    }

    free(item->owned_session_id);
    free(item->owned_user_text);
    free(item->owned_source_channel);
    free(item->owned_source_chat_id);
    free(item->owned_source_sender_id);
    free(item->owned_source_message_id);
    free(item->owned_source_cap);
    free(item->owned_target_channel);
    free(item->owned_target_chat_id);
    memset(item, 0, sizeof(*item));
}

static esp_err_t clone_request_item(const claw_core_request_t *request,
                                    claw_core_request_item_t *out_item)
{
    claw_core_request_item_t item = {0};

    if (!request || !out_item || !request->user_text || request->user_text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    item.view.request_id = request->request_id;
    item.view.flags = request->flags;
    item.owned_session_id = claw_core_dup_string(request->session_id);
    item.owned_user_text = claw_core_dup_string(request->user_text);
    item.owned_source_channel = claw_core_dup_string(request->source_channel);
    item.owned_source_chat_id = claw_core_dup_string(request->source_chat_id);
    item.owned_source_sender_id = claw_core_dup_string(request->source_sender_id);
    item.owned_source_message_id = claw_core_dup_string(request->source_message_id);
    item.owned_source_cap = claw_core_dup_string(request->source_cap);
    item.owned_target_channel = claw_core_dup_string(request->target_channel);
    item.owned_target_chat_id = claw_core_dup_string(request->target_chat_id);

    item.view.session_id = item.owned_session_id;
    item.view.user_text = item.owned_user_text;
    item.view.source_channel = item.owned_source_channel;
    item.view.source_chat_id = item.owned_source_chat_id;
    item.view.source_sender_id = item.owned_source_sender_id;
    item.view.source_message_id = item.owned_source_message_id;
    item.view.source_cap = item.owned_source_cap;
    item.view.target_channel = item.owned_target_channel;
    item.view.target_chat_id = item.owned_target_chat_id;

    if ((request->session_id && !item.owned_session_id) ||
            (request->source_channel && !item.owned_source_channel) ||
            (request->source_chat_id && !item.owned_source_chat_id) ||
            (request->source_sender_id && !item.owned_source_sender_id) ||
            (request->source_message_id && !item.owned_source_message_id) ||
            (request->source_cap && !item.owned_source_cap) ||
            (request->target_channel && !item.owned_target_channel) ||
            (request->target_chat_id && !item.owned_target_chat_id) ||
            !item.owned_user_text) {
        claw_core_free_request_item(&item);
        return ESP_ERR_NO_MEM;
    }

    *out_item = item;
    return ESP_OK;
}

static esp_err_t queue_insert_request_locked(claw_core_state_t *core,
                                             claw_core_request_item_t *item)
{
    size_t tail;

    if (!core || !item || !item->view.session_id || item->view.session_id[0] == '\0' ||
            core->inflight_request_id == 0 ||
            core->inflight_session_id[0] == '\0' ||
            strcmp(core->inflight_session_id, item->view.session_id) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!claw_core_agent_loop_phase_is_insertable(core->agent_loop_phase)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (core->insert_queue_count >= CLAW_CORE_INSERT_QUEUE_LEN) {
        return ESP_ERR_NO_MEM;
    }

    tail = (core->insert_queue_head + core->insert_queue_count) % CLAW_CORE_INSERT_QUEUE_LEN;
    core->insert_queue[tail] = *item;
    memset(item, 0, sizeof(*item));
    core->insert_queue_count++;

    if (core->agent_loop_phase == CLAW_CORE_AGENT_LOOP_PHASE_IN_LLM_HTTP &&
            core->inflight_abort_reason != CLAW_CORE_CONTROL_ABORT_REASON_CANCEL) {
        core->inflight_abort = true;
        core->inflight_abort_reason = CLAW_CORE_CONTROL_ABORT_REASON_USER_INTERRUPT;
    }
    return ESP_OK;
}

static esp_err_t try_queue_insert_request(claw_core_state_t *core,
                                          claw_core_request_item_t *item)
{
    uint32_t inserted_request_id;
    uint32_t inflight_request_id;
    char session_id[CLAW_CORE_INFLIGHT_SESSION_ID_SIZE] = {0};
    size_t depth;
    esp_err_t err;

    if (!core || !core->inflight_lock || !item) {
        return ESP_ERR_INVALID_STATE;
    }

    inserted_request_id = item->view.request_id;
    if (item->view.session_id) {
        strlcpy(session_id, item->view.session_id, sizeof(session_id));
    }

    if (xSemaphoreTake(core->inflight_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    inflight_request_id = core->inflight_request_id;
    err = queue_insert_request_locked(core, item);
    depth = core->insert_queue_count;
    xSemaphoreGive(core->inflight_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "inserted user input request=%" PRIu32 " into inflight_request=%" PRIu32
                 " session=%s depth=%u",
                 inserted_request_id,
                 inflight_request_id,
                 session_id,
                 (unsigned)depth);
    }
    return err;
}

esp_err_t claw_core_ingress_submit(claw_core_state_t *core,
                                   const claw_core_request_t *request,
                                   uint32_t timeout_ms)
{
    claw_core_request_item_t item = {0};
    TickType_t ticks;
    esp_err_t err;

    if (!core || !core->started || !request || !request->user_text || request->user_text[0] == '\0') {
        return (core && core->started) ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }

    err = clone_request_item(request, &item);
    if (err != ESP_OK) {
        return err;
    }

    if (request->flags & CLAW_CORE_REQUEST_FLAG_USER_INTERRUPT) {
        err = try_queue_insert_request(core, &item);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err == ESP_ERR_NO_MEM || err == ESP_ERR_TIMEOUT) {
            claw_core_free_request_item(&item);
            return err;
        }
    }

    ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueSend(core->request_queue, &item, ticks) != pdTRUE) {
        claw_core_free_request_item(&item);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

bool claw_core_ingress_dequeue_inserted_user_inputs(claw_core_state_t *core,
                                                    const char *session_id,
                                                    char **texts,
                                                    size_t max_count,
                                                    size_t *out_count)
{
    bool found = false;

    if (!core || !session_id || session_id[0] == '\0' || !texts || max_count == 0 || !out_count) {
        return false;
    }
    *out_count = 0;
    if (!core->inflight_lock) {
        return false;
    }

    if (xSemaphoreTake(core->inflight_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    while (core->insert_queue_count > 0 && *out_count < max_count) {
        claw_core_request_item_t *item = &core->insert_queue[core->insert_queue_head];

        if (!item->view.session_id || strcmp(item->view.session_id, session_id) != 0) {
            break;
        }
        texts[*out_count] = item->owned_user_text;
        item->owned_user_text = NULL;
        item->view.user_text = NULL;
        claw_core_free_request_item(item);
        core->insert_queue_head = (core->insert_queue_head + 1) % CLAW_CORE_INSERT_QUEUE_LEN;
        core->insert_queue_count--;
        (*out_count)++;
        found = true;
    }
    if (core->insert_queue_count == 0) {
        core->insert_queue_head = 0;
    }
    xSemaphoreGive(core->inflight_lock);
    return found;
}

void claw_core_ingress_clear_insert_queue_locked(claw_core_state_t *core)
{
    size_t i;

    if (!core) {
        return;
    }

    for (i = 0; i < core->insert_queue_count; i++) {
        size_t index = (core->insert_queue_head + i) % CLAW_CORE_INSERT_QUEUE_LEN;

        claw_core_free_request_item(&core->insert_queue[index]);
    }
    core->insert_queue_head = 0;
    core->insert_queue_count = 0;
}

void claw_core_free_response_item(claw_core_response_item_t *item)
{
    if (!item) {
        return;
    }

    free(item->view.target_channel);
    free(item->view.target_chat_id);
    free(item->view.text);
    free(item->view.error_message);
    memset(item, 0, sizeof(*item));
}

esp_err_t claw_core_response_push(claw_core_state_t *core, claw_core_response_item_t *item)
{
    if (!core || !core->response_queue || !item) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueSend(core->response_queue, item, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    item->view.target_channel = NULL;
    item->view.target_chat_id = NULL;
    item->view.text = NULL;
    item->view.error_message = NULL;
    return ESP_OK;
}

static esp_err_t enqueue_pending_response(claw_core_state_t *core,
                                          claw_core_response_item_t *item)
{
    claw_core_pending_response_t *node = calloc(1, sizeof(*node));

    if (!core || !item) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!node) {
        return ESP_ERR_NO_MEM;
    }

    node->item = *item;
    if (!core->pending_tail) {
        core->pending_head = node;
    } else {
        core->pending_tail->next = node;
    }
    core->pending_tail = node;
    memset(item, 0, sizeof(*item));
    return ESP_OK;
}

static bool pop_pending_response(claw_core_state_t *core,
                                 uint32_t request_id,
                                 bool match_any,
                                 claw_core_response_item_t *out_item)
{
    claw_core_pending_response_t *prev = NULL;
    claw_core_pending_response_t *cur;

    if (!core || !out_item) {
        return false;
    }

    cur = core->pending_head;
    while (cur) {
        if (match_any || cur->item.view.request_id == request_id) {
            if (prev) {
                prev->next = cur->next;
            } else {
                core->pending_head = cur->next;
            }
            if (core->pending_tail == cur) {
                core->pending_tail = prev;
            }
            *out_item = cur->item;
            free(cur);
            return true;
        }
        prev = cur;
        cur = cur->next;
    }

    return false;
}

static void move_response_item(claw_core_response_t *dst, claw_core_response_item_t *src)
{
    memset(dst, 0, sizeof(*dst));
    *dst = src->view;
    memset(src, 0, sizeof(*src));
}

esp_err_t claw_core_response_receive_for(claw_core_state_t *core,
                                         uint32_t request_id,
                                         claw_core_response_t *response,
                                         uint32_t timeout_ms)
{
    claw_core_response_item_t item = {0};
    TickType_t start_ticks;
    bool match_any;

    if (!core || !core->started || !response) {
        return (core && core->started) ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(core->response_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    start_ticks = xTaskGetTickCount();
    match_any = (request_id == 0);

    if (pop_pending_response(core, request_id, match_any, &item)) {
        xSemaphoreGive(core->response_lock);
        move_response_item(response, &item);
        return ESP_OK;
    }

    while (true) {
        TickType_t wait_ticks;
        TickType_t elapsed = xTaskGetTickCount() - start_ticks;

        if (timeout_ms == UINT32_MAX) {
            wait_ticks = portMAX_DELAY;
        } else {
            TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

            if (elapsed >= timeout_ticks) {
                xSemaphoreGive(core->response_lock);
                return ESP_ERR_TIMEOUT;
            }
            wait_ticks = timeout_ticks - elapsed;
        }

        if (xQueueReceive(core->response_queue, &item, wait_ticks) != pdTRUE) {
            xSemaphoreGive(core->response_lock);
            return ESP_ERR_TIMEOUT;
        }

        if (match_any || item.view.request_id == request_id) {
            xSemaphoreGive(core->response_lock);
            move_response_item(response, &item);
            return ESP_OK;
        }

        if (enqueue_pending_response(core, &item) != ESP_OK) {
            claw_core_free_response_item(&item);
        }
    }
}

void claw_core_response_free(claw_core_response_t *response)
{
    if (!response) {
        return;
    }

    free(response->target_channel);
    free(response->target_chat_id);
    free(response->text);
    free(response->error_message);
    response->target_channel = NULL;
    response->target_chat_id = NULL;
    response->text = NULL;
    response->error_message = NULL;
}
