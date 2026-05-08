/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_unitv_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"

#define UNITV_SCAN_RESP_MAX            768
#define UNITV_CAPTURE_ANALYSIS_MAX     1024
#define UNITV_CAPTURE_DEFAULT_QUALITY  30

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static esp_err_t cap_unitv_scan_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    char mode[16] = "RELIABLE";
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (args) {
        cJSON *m = cJSON_GetObjectItem(args, "mode");
        if (cJSON_IsString(m) && m->valuestring &&
                (strcmp(m->valuestring, "fast") == 0 || strcmp(m->valuestring, "FAST") == 0)) {
            strlcpy(mode, "FAST", sizeof(mode));
        }
        cJSON_Delete(args);
    }

    char cmd_args[64] = {0};
    snprintf(cmd_args, sizeof(cmd_args), "{\"mode\":\"%s\",\"frames\":1}", mode);

    char raw[UNITV_SCAN_RESP_MAX] = {0};
    esp_err_t err = cap_unitv_uart_cmd("SCAN", cmd_args, raw, sizeof(raw),
                                       g_cap_unitv.cfg.default_timeout_ms);
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"status\":\"camera_unavailable\",\"action\":\"unitv_scan\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(raw);
    if (!root) {
        snprintf(output, output_size, "{\"status\":\"invalid_response\",\"action\":\"unitv_scan\"}");
        return ESP_OK;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    char *result_str = result ? cJSON_PrintUnformatted(result) : NULL;
    snprintf(output, output_size,
             "{\"status\":\"ok\",\"action\":\"unitv_scan\",\"result\":%s}",
             result_str ? result_str : "{}");
    free(result_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_unitv_capture_execute(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output,
                                           size_t output_size)
{
    int quality = UNITV_CAPTURE_DEFAULT_QUALITY;
    char question[240] = "Describe what is visible in this rover camera image. Be concise and concrete.";
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (args) {
        cJSON *q = cJSON_GetObjectItem(args, "quality");
        if (cJSON_IsNumber(q)) {
            quality = q->valueint;
        }
        cJSON *prompt = cJSON_GetObjectItem(args, "question");
        if (cJSON_IsString(prompt) && prompt->valuestring && prompt->valuestring[0]) {
            strlcpy(question, prompt->valuestring, sizeof(question));
        }
        cJSON_Delete(args);
    }
    quality = clamp_int(quality, 30, 95);

    if (!g_cap_unitv.vision_configured) {
        snprintf(output, output_size, "{\"status\":\"vision_not_configured\",\"action\":\"unitv_capture\"}");
        return ESP_OK;
    }

    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    esp_err_t err = cap_unitv_uart_capture_jpeg(quality, &jpeg, &jpeg_size);
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"status\":\"camera_capture_failed\",\"action\":\"unitv_capture\"}");
        return ESP_OK;
    }

    char analysis[UNITV_CAPTURE_ANALYSIS_MAX] = {0};
    err = cap_unitv_vision_call(question, jpeg, jpeg_size, analysis, sizeof(analysis));
    free(jpeg);
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "{\"status\":\"vision_failed\",\"action\":\"unitv_capture\",\"jpeg_bytes\":%u}",
                 (unsigned)jpeg_size);
        return ESP_OK;
    }

    cJSON *out = cJSON_CreateObject();
    if (!out) {
        snprintf(output, output_size, "{\"status\":\"memory_error\",\"action\":\"unitv_capture\"}");
        return ESP_OK;
    }
    cJSON_AddStringToObject(out, "status", "ok");
    cJSON_AddStringToObject(out, "action", "unitv_capture");
    cJSON_AddNumberToObject(out, "jpeg_bytes", (double)jpeg_size);
    cJSON_AddStringToObject(out, "analysis", analysis);
    char *json_str = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (json_str) {
        strlcpy(output, json_str, output_size);
        free(json_str);
    } else {
        snprintf(output, output_size, "{\"status\":\"memory_error\",\"action\":\"unitv_capture\"}");
    }
    return ESP_OK;
}

static const claw_cap_descriptor_t s_unitv_descriptors[] = {
    {
        .id = "unitv_scan",
        .name = "unitv_scan",
        .family = "vision",
        .description = "Run the UnitV onboard scan and return structured vision output.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"mode\":{\"type\":\"string\",\"enum\":[\"fast\",\"reliable\"]}"
            "}}",
        .execute = cap_unitv_scan_execute,
    },
    {
        .id = "unitv_capture",
        .name = "unitv_capture",
        .family = "vision",
        .description = "Capture a JPEG from UnitV and ask a vision LLM to analyze it.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"question\":{\"type\":\"string\"},"
            "\"quality\":{\"type\":\"integer\",\"minimum\":25,\"maximum\":35,"
            "\"description\":\"JPEG quality 25-35. Use 25 for fast scan, 35 for more detail. Keep low due to memory constraints.\"}"
            "}}",
        .execute = cap_unitv_capture_execute,
    },
};

static const claw_cap_group_t s_unitv_group = {
    .group_id = "cap_unitv",
    .plugin_name = "cap_unitv",
    .descriptors = s_unitv_descriptors,
    .descriptor_count = sizeof(s_unitv_descriptors) / sizeof(s_unitv_descriptors[0]),
};

esp_err_t cap_unitv_register_group(void)
{
    if (claw_cap_group_exists(s_unitv_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_unitv_group);
}
