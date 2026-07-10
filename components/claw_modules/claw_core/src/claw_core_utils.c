/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_utils_string.h"
#include "esp_log.h"

static const char *TAG = "claw_core";

const char *claw_core_log_snippet(const char *text)
{
    return text ? text : "";
}

int claw_core_log_snippet_len(const char *text)
{
    if (!text) {
        return 0;
    }
#if CLAW_CORE_LOG_SNIPPET_LEN == 0
    return (int)strlen(text);
#else
    size_t len = strlen(text);
    return (int)(len > CLAW_CORE_LOG_SNIPPET_LEN ?
                 claw_utils_utf8_prefix_len(text, CLAW_CORE_LOG_SNIPPET_LEN) : len);
#endif
}

const char *claw_core_log_snippet_suffix(const char *text)
{
#if CLAW_CORE_LOG_SNIPPET_LEN == 0
    (void)text;
    return "";
#else
    return text && strlen(text) > CLAW_CORE_LOG_SNIPPET_LEN ? "..." : "";
#endif
}

const char *claw_core_context_kind_to_string(claw_core_context_kind_t kind)
{
    switch (kind) {
    case CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT:
        return "system_prompt";
    case CLAW_CORE_CONTEXT_KIND_MESSAGES:
        return "messages";
    case CLAW_CORE_CONTEXT_KIND_TOOLS:
        return "tools";
    default:
        return "unknown";
    }
}

esp_err_t claw_core_append_tool_summary_line(char *summary,
                                             size_t summary_size,
                                             const char *tool_name,
                                             bool ok)
{
    size_t used;
    int written;

    if (!summary || summary_size == 0 || !tool_name || !tool_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    used = strlen(summary);
    if (used >= summary_size - 1) {
        return ESP_ERR_NO_MEM;
    }

    written = snprintf(summary + used,
                       summary_size - used,
                       "%s- %s: %s\n",
                       used == 0 ? "[tool_calls]\n" : "",
                       tool_name,
                       ok ? "ok" : "failed");
    if (written < 0 || (size_t)written >= summary_size - used) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static bool obs_csv_contains(const char *csv, const char *name)
{
    const char *needle = name;
    const char *p = csv;
    size_t need_len;

    if (!csv || !name) {
        return false;
    }
    need_len = strlen(needle);
    while (*p) {
        if (strncmp(p, needle, need_len) == 0 && (p[need_len] == ',' || p[need_len] == '\0')) {
            return true;
        }
        const char *next = strchr(p, ',');
        if (!next) {
            break;
        }
        p = next + 1;
    }
    return false;
}

void claw_core_obs_csv_append(char *csv, size_t csv_size, const char *name, bool dedup)
{
    size_t cur;
    int written;

    if (!csv || csv_size == 0 || !name || !name[0]) {
        return;
    }
    if (dedup && obs_csv_contains(csv, name)) {
        return;
    }
    cur = strlen(csv);
    if (cur >= csv_size - 1) {
        return;
    }
    written = snprintf(csv + cur, csv_size - cur, "%s%s", cur == 0 ? "" : ",", name);
    if (written < 0 || (size_t)written >= csv_size - cur) {
        csv[csv_size - 1] = '\0';
    }
}

void claw_core_log_tool_call_names(uint32_t request_id, const claw_core_llm_response_t *response)
{
    char buf[192] = {0};
    size_t off = 0;
    size_t i;

    if (!response) {
        return;
    }

    if (response->reasoning_content && response->reasoning_content[0]) {
        ESP_LOGD(TAG, "llm_reasoning_content request=%" PRIu32 " content=%.*s%s",
                 request_id,
                 claw_core_log_snippet_len(response->reasoning_content),
                 claw_core_log_snippet(response->reasoning_content),
                 claw_core_log_snippet_suffix(response->reasoning_content));
        return;
    }

    if (response->tool_call_count == 0) {
        return;
    }

    for (i = 0; i < response->tool_call_count; i++) {
        const char *name = response->tool_calls[i].name ? response->tool_calls[i].name : "(null)";
        int written = snprintf(buf + off,
                               sizeof(buf) - off,
                               "%s%s",
                               i == 0 ? "" : ",",
                               name);

        if (written < 0 || (size_t)written >= sizeof(buf) - off) {
            off = sizeof(buf) - 1;
            break;
        }
        off += (size_t)written;
    }

    ESP_LOGD(TAG, "llm_tool_calls request=%" PRIu32 " count=%u names=%s%s",
             request_id,
             (unsigned)response->tool_call_count,
             buf,
             off >= sizeof(buf) - 1 ? "..." : "");
}

void claw_core_check_timezone(void)
{
    const char *timezone = getenv("TZ");

    if (!timezone || timezone[0] == '\0') {
        ESP_LOGW(TAG, "Timezone is not configured; time-related responses may use an unexpected timezone");
    }
}
