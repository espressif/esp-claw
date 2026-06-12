/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_internal.h"
#include "claw_task.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static void free_context_provider_storage(claw_core_state_t *core)
{
    size_t i;

    if (!core) {
        return;
    }

    for (i = 0; i < core->context_provider_count; i++) {
        free((char *)core->context_providers[i].name);
        core->context_providers[i].name = NULL;
    }
    free(core->context_providers);
    core->context_providers = NULL;
    core->context_provider_count = 0;
    core->context_provider_capacity = 0;
}

static void claw_core_free_runtime(claw_core_state_t *core)
{
    if (!core) {
        return;
    }

    free_context_provider_storage(core);
    free(core->system_prompt);
    claw_core_llm_config_free(&core->llm_config);
    claw_llm_runtime_deinit(core->llm_runtime);
    if (core->llm_lock) {
        vSemaphoreDelete(core->llm_lock);
    }
    if (core->request_queue) {
        vQueueDelete(core->request_queue);
    }
    if (core->response_queue) {
        vQueueDelete(core->response_queue);
    }
    if (core->response_lock) {
        vSemaphoreDelete(core->response_lock);
    }
    if (core->inflight_lock) {
        if (xSemaphoreTake(core->inflight_lock, portMAX_DELAY) == pdTRUE) {
            claw_core_ingress_clear_insert_queue_locked(core);
            xSemaphoreGive(core->inflight_lock);
        }
        vSemaphoreDelete(core->inflight_lock);
    }
    memset(core, 0, sizeof(*core));
    free(core);
}

esp_err_t claw_core_create(const claw_core_config_t *config, claw_core_handle_t *out_core)
{
    claw_core_state_t *core = NULL;
    claw_core_llm_config_t llm_config = {0};
    esp_err_t err;
    uint32_t request_queue_len;
    uint32_t response_queue_len;

    if (out_core) {
        *out_core = NULL;
    }
    if (!config || !config->system_prompt || !out_core) {
        return ESP_ERR_INVALID_ARG;
    }

    core = calloc(1, sizeof(*core));
    if (!core) {
        return ESP_ERR_NO_MEM;
    }
    claw_core_check_timezone();

    core->instance_id = config->instance_id;
    snprintf(core->log_tag, sizeof(core->log_tag), "claw_core_agent_%" PRIu32, core->instance_id);
    core->system_prompt = claw_utils_string_dup(config->system_prompt);
    if (!core->system_prompt) {
        claw_core_free_runtime(core);
        return ESP_ERR_NO_MEM;
    }
    core->persist_context = config->persist_context;
    core->persist_context_user_ctx = config->persist_context_user_ctx;
    core->request_gate = config->request_gate;
    core->request_gate_user_ctx = config->request_gate_user_ctx;
    core->on_request_start = config->on_request_start;
    core->on_request_start_user_ctx = config->on_request_start_user_ctx;
    core->collect_stage_note = config->collect_stage_note;
    core->collect_stage_note_user_ctx = config->collect_stage_note_user_ctx;
    core->call_cap = config->call_cap;
    core->cap_user_ctx = config->cap_user_ctx;

    request_queue_len = config->request_queue_len ? config->request_queue_len : CLAW_CORE_DEFAULT_REQUEST_Q;
    response_queue_len = config->response_queue_len ? config->response_queue_len : CLAW_CORE_DEFAULT_RESPONSE_Q;
    core->task_stack_size = config->task_stack_size ? config->task_stack_size : CLAW_CORE_DEFAULT_STACK_SIZE;
    core->task_priority = config->task_priority ? config->task_priority : CLAW_CORE_DEFAULT_PRIORITY;
    core->task_core = config->task_core;
    core->max_tool_iterations = config->max_tool_iterations ?
                                  config->max_tool_iterations : CLAW_CORE_DEFAULT_TOOL_ITERATIONS;
    core->context_provider_capacity = config->max_context_providers;

    if (core->context_provider_capacity > 0) {
        core->context_providers = calloc(core->context_provider_capacity,
                                         sizeof(claw_core_context_provider_t));
        if (!core->context_providers) {
            claw_core_free_runtime(core);
            return ESP_ERR_NO_MEM;
        }
    }

    core->request_queue = xQueueCreate(request_queue_len, sizeof(claw_core_request_item_t));
    core->response_queue = xQueueCreate(response_queue_len, sizeof(claw_core_response_item_t));
    core->response_lock = xSemaphoreCreateMutex();
    core->inflight_lock = xSemaphoreCreateMutex();
    core->llm_lock = xSemaphoreCreateMutex();
    if (!core->request_queue || !core->response_queue ||
            !core->response_lock || !core->inflight_lock || !core->llm_lock) {
        claw_core_free_runtime(core);
        return ESP_ERR_NO_MEM;
    }

    llm_config.api_key = config->api_key;
    llm_config.backend_type = config->backend_type;
    llm_config.model = config->model;
    llm_config.base_url = config->base_url;
    llm_config.auth_type = config->auth_type;
    llm_config.max_tokens_field = config->max_tokens_field;
    llm_config.timeout_ms = config->timeout_ms;
    llm_config.max_tokens = config->max_tokens;
    llm_config.image_max_bytes = config->image_max_bytes;
    llm_config.supports_tools = config->supports_tools;
    llm_config.supports_vision = config->supports_vision;
    llm_config.image_remote_url_only = config->image_remote_url_only;
    err = claw_core_llm_config_copy(&core->llm_config, &llm_config);
    if (err != ESP_OK) {
        claw_core_free_runtime(core);
        return err;
    }

    core->initialized = true;
    *out_core = core;
    ESP_LOGI(core->log_tag, "Initialized");
    return ESP_OK;
}

esp_err_t claw_core_update_llm_config(claw_core_handle_t core,
                                      const claw_core_config_t *config)
{
    claw_core_llm_config_t next = {0};
    claw_core_llm_config_t copied = {0};
    esp_err_t err;

    if (!core || !core->initialized || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    next.api_key = config->api_key;
    next.backend_type = config->backend_type;
    next.model = config->model;
    next.base_url = config->base_url;
    next.auth_type = config->auth_type;
    next.max_tokens_field = config->max_tokens_field;
    next.timeout_ms = config->timeout_ms;
    next.max_tokens = config->max_tokens;
    next.image_max_bytes = config->image_max_bytes;
    next.supports_tools = config->supports_tools;
    next.supports_vision = config->supports_vision;
    next.image_remote_url_only = config->image_remote_url_only;

    err = claw_core_llm_config_copy(&copied, &next);
    if (err != ESP_OK) {
        return err;
    }

    if (core->llm_lock) {
        xSemaphoreTake(core->llm_lock, portMAX_DELAY);
    }
    claw_llm_runtime_deinit(core->llm_runtime);
    core->llm_runtime = NULL;
    claw_core_llm_config_free(&core->llm_config);
    core->llm_config = copied;
    if (core->llm_lock) {
        xSemaphoreGive(core->llm_lock);
    }

    ESP_LOGI(core->log_tag,
             "LLM config updated backend=%s base_url=%s model=%s token=%s",
             core->llm_config.backend_type && core->llm_config.backend_type[0] ?
             core->llm_config.backend_type : "(empty)",
             core->llm_config.base_url && core->llm_config.base_url[0] ?
             core->llm_config.base_url : "(empty)",
             core->llm_config.model && core->llm_config.model[0] ?
             core->llm_config.model : "(empty)",
             core->llm_config.api_key && core->llm_config.api_key[0] ?
             "configured" : "missing");
    return ESP_OK;
}

esp_err_t claw_core_start(claw_core_handle_t core)
{
    BaseType_t task_result;

    if (!core || !core->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (core->started) {
        return ESP_OK;
    }

    task_result = claw_task_create(&(claw_task_config_t){
                                        .name = core->log_tag,
                                        .stack_size = core->task_stack_size,
                                        .priority = core->task_priority,
                                        .core_id = core->task_core,
                                        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                                    },
                                    claw_core_agent_loop_task,
                                    core,
                                    &core->task_handle);

    if (task_result != pdPASS) {
        return ESP_FAIL;
    }

    core->started = true;
    ESP_LOGI(core->log_tag, "Started worker task");
    return ESP_OK;
}

esp_err_t claw_core_stop(claw_core_handle_t core, uint32_t timeout_ms)
{
    claw_core_request_item_t wake = {0};
    TickType_t deadline;

    if (!core || !core->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!core->started) {
        return ESP_OK;
    }

    core->stop_requested = true;
    (void)claw_core_cancel_request(core, 0);
    if (xQueueSend(core->request_queue, &wake, 0) != pdTRUE) {
        ESP_LOGW(core->log_tag, "Failed to enqueue stop wake item");
    }

    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms ? timeout_ms : 5000);
    while (core->started && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (core->started) {
        ESP_LOGE(core->log_tag, "Stop timed out");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t claw_core_destroy(claw_core_handle_t core)
{
    if (!core || !core->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (core->started) {
        esp_err_t err = claw_core_stop(core, 5000);

        if (err != ESP_OK) {
            ESP_LOGE(core->log_tag, "Destroy rejected: stop failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    claw_core_free_runtime(core);
    return ESP_OK;
}

esp_err_t claw_core_add_context_provider(claw_core_handle_t core,
                                         const claw_core_context_provider_t *provider)
{
    claw_core_context_provider_t *slot = NULL;

    if (!core || !core->initialized || core->started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!provider || !provider->name || !provider->collect) {
        return ESP_ERR_INVALID_ARG;
    }
    if (core->context_provider_count >= core->context_provider_capacity) {
        return ESP_ERR_NO_MEM;
    }

    slot = &core->context_providers[core->context_provider_count];
    slot->name = claw_utils_string_dup(provider->name);
    if (!slot->name) {
        return ESP_ERR_NO_MEM;
    }
    slot->collect = provider->collect;
    slot->user_ctx = provider->user_ctx;
    slot->flags = provider->flags;
    core->context_provider_count++;
    return ESP_OK;
}

esp_err_t claw_core_add_completion_observer(claw_core_handle_t core,
                                            claw_core_completion_observer_fn observer,
                                            void *user_ctx)
{
    if (!core || !core->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!observer) {
        return ESP_ERR_INVALID_ARG;
    }
    if (core->completion_observer_count >= CLAW_CORE_MAX_COMPLETION_OBSERVERS) {
        return ESP_ERR_NO_MEM;
    }
    core->completion_observers[core->completion_observer_count].fn = observer;
    core->completion_observers[core->completion_observer_count].user_ctx = user_ctx;
    core->completion_observer_count++;
    return ESP_OK;
}

esp_err_t claw_core_cancel_request(claw_core_handle_t core, uint32_t request_id)
{
    return claw_core_control_cancel_request(core, request_id);
}

claw_core_agent_loop_phase_t claw_core_get_agent_loop_phase(claw_core_handle_t core)
{
    return claw_core_control_get_phase(core);
}

esp_err_t claw_core_submit(claw_core_handle_t core,
                           const claw_core_request_t *request,
                           uint32_t timeout_ms)
{
    return claw_core_ingress_submit(core, request, timeout_ms);
}

esp_err_t claw_core_receive(claw_core_handle_t core,
                            claw_core_response_t *response,
                            uint32_t timeout_ms)
{
    return claw_core_receive_for(core, 0, response, timeout_ms);
}

esp_err_t claw_core_receive_for(claw_core_handle_t core,
                                uint32_t request_id,
                                claw_core_response_t *response,
                                uint32_t timeout_ms)
{
    return claw_core_response_receive_for(core, request_id, response, timeout_ms);
}
