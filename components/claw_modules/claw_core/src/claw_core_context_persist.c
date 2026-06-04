/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_internal.h"

#include <inttypes.h>
#include <stdlib.h>

#include "esp_log.h"

static const char *TAG = "claw_core";

static esp_err_t persist_context_batch_if_configured(claw_core_state_t *core,
                                                     const claw_core_request_t *request,
                                                     const claw_core_context_record_t *records,
                                                     size_t record_count,
                                                     bool turn_completed)
{
    claw_core_context_persist_batch_t batch = {0};

    if (!core || !core->persist_context ||
            !request || !request->session_id || !request->session_id[0]) {
        return ESP_OK;
    }
    if (!records || record_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    batch.session_id = request->session_id;
    batch.request = request;
    batch.records = records;
    batch.record_count = record_count;
    batch.turn_completed = turn_completed;

    return core->persist_context(&batch, core->persist_context_user_ctx);
}

void claw_core_log_context_persist_failure(const claw_core_request_t *request,
                                           const char *operation,
                                           esp_err_t err)
{
    if (!request || err == ESP_OK) {
        return;
    }

    ESP_LOGW(TAG,
             "%s failed for request=%" PRIu32 ": %s",
             operation ? operation : "persist_context_records",
             request->request_id,
             esp_err_to_name(err));
}

static bool request_has_context_persistence(claw_core_state_t *core,
                                            const claw_core_request_t *request)
{
    return core && core->persist_context &&
           request && request->session_id && request->session_id[0];
}

esp_err_t claw_core_persist_context_user_messages_if_configured(claw_core_state_t *core,
                                                                const claw_core_request_t *request,
                                                                const char *const *texts,
                                                                size_t text_count,
                                                                bool *out_persisted)
{
    claw_core_context_record_t records[CLAW_CORE_INSERT_QUEUE_LEN];
    size_t i;

    if (out_persisted) {
        *out_persisted = false;
    }
    if (!request || !texts || text_count == 0 || text_count > CLAW_CORE_INSERT_QUEUE_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!request_has_context_persistence(core, request)) {
        return ESP_OK;
    }

    for (i = 0; i < text_count; i++) {
        if (!texts[i] || !texts[i][0]) {
            return ESP_ERR_INVALID_ARG;
        }
        records[i] = (claw_core_context_record_t) {
            .type = CLAW_CORE_CONTEXT_RECORD_USER,
            .text = texts[i],
        };
    }

    esp_err_t err = persist_context_batch_if_configured(core, request, records, text_count, false);

    if (err == ESP_OK && out_persisted) {
        *out_persisted = true;
    }
    return err;
}

esp_err_t claw_core_persist_context_tool_round_if_configured(
    claw_core_state_t *core,
    const claw_core_request_t *request,
    const char *assistant_tool_message_json,
    const char *tool_results_json)
{
    claw_core_context_record_t records[2];
    size_t record_count = 0;

    if (!request) {
        return ESP_OK;
    }
    if (!assistant_tool_message_json || !assistant_tool_message_json[0] ||
            !tool_results_json || !tool_results_json[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    records[record_count++] = (claw_core_context_record_t) {
        .type = CLAW_CORE_CONTEXT_RECORD_ASSISTANT_TOOL,
        .message_json = assistant_tool_message_json,
    };
    records[record_count++] = (claw_core_context_record_t) {
        .type = CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT,
        .message_json = tool_results_json,
    };

    return persist_context_batch_if_configured(core, request, records, record_count, false);
}

esp_err_t claw_core_persist_context_final_if_configured(claw_core_state_t *core,
                                                        const claw_core_request_t *request,
                                                        const char *assistant_final_json,
                                                        const char *assistant_text)
{
    claw_core_context_record_t records[1];
    size_t record_count = 0;

    if (!request) {
        return ESP_OK;
    }

    records[record_count++] = (claw_core_context_record_t) {
        .type = CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL,
        .message_json = assistant_final_json,
        .text = assistant_text,
    };

    return persist_context_batch_if_configured(core, request, records, record_count, true);
}

char *claw_core_build_context_failure_trace(const char *error_message,
                                            const char *tool_summary)
{
    const char *reason = (error_message && error_message[0]) ? error_message : "unknown error";

    if (tool_summary && tool_summary[0]) {
        return claw_core_dup_printf("Session note: the previous request failed before producing a final answer.\n"
                                    "Reason: %s\n%s",
                                    reason,
                                    tool_summary);
    }

    return claw_core_dup_printf("Session note: the previous request failed before producing a final answer.\n"
                                "Reason: %s",
                                reason);
}
