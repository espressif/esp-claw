/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_voice.h"
#include "cap_voice_pipeline.h"
#include "cap_voice_tts.h"
#include "cap_voice_audio.h"
#include "claw_cap.h"
#include "claw_event.h"
#include "claw_event_publisher.h"
#include "esp_check.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "cap_voice";
static cap_voice_config_t s_config;
static char s_session_voice[32];  /* mutable copy for voice_set_voice tool */

static void ui_notify(cap_voice_ui_state_t state)
{
    if (s_config.on_ui_state) {
        s_config.on_ui_state(state, s_config.ui_ctx);
    }
}

/* Callbacks from pipeline -> publish claw events */
static void on_wakeup(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Wake word detected");
    ui_notify(CAP_VOICE_UI_LISTENING);
}

static void on_command(const char *cmd_id, const char *text, void *ctx)
{
    (void)ctx;
    if (!text) return;
    ESP_LOGI(TAG, "Fast-path command: %s", text);
    claw_event_t ev = {0};
    strlcpy(ev.event_id,       "voice-direct",  sizeof(ev.event_id));
    strlcpy(ev.source_cap,     "cap_voice",     sizeof(ev.source_cap));
    strlcpy(ev.event_type,     "voice_command", sizeof(ev.event_type));
    strlcpy(ev.source_channel, "voice",         sizeof(ev.source_channel));
    strlcpy(ev.content_type,   "direct",        sizeof(ev.content_type));
    ev.text = (char *)text;
    claw_event_router_publish(&ev);
}

static void on_transcript(const char *cmd_id, const char *text, void *ctx)
{
    (void)ctx;
    if (!text || !text[0]) return;
    ESP_LOGI(TAG, "Transcript: %s", text);
    ui_notify(CAP_VOICE_UI_THINKING);
    claw_event_t ev = {0};
    strlcpy(ev.event_id,       "voice-input",  sizeof(ev.event_id));
    strlcpy(ev.source_cap,     "cap_voice",    sizeof(ev.source_cap));
    strlcpy(ev.event_type,     "voice_input",  sizeof(ev.event_type));
    strlcpy(ev.source_channel, "voice",        sizeof(ev.source_channel));
    strlcpy(ev.target_channel, "voice",        sizeof(ev.target_channel));
    ev.text = (char *)text;
    claw_event_router_publish(&ev);
}

/* LLM tool: voice_say */
static esp_err_t voice_say_execute(const char *input_json,
                                    const claw_cap_call_context_t *ctx,
                                    char *output,
                                    size_t output_size)
{
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    (void)ctx;

    if (!output || output_size == 0) {
        cJSON_Delete(args);
        return ESP_ERR_INVALID_ARG;
    }

    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(args, "text"));
    if (!text) {
        cJSON_Delete(args);
        snprintf(output, output_size, "{\"error\":\"missing text\"}");
        return ESP_OK;
    }

    cap_voice_tts_config_t tcfg = {
        .api_key  = s_config.tts_api_key ? s_config.tts_api_key : "",
        .base_url = s_config.tts_base_url,
        .voice    = s_config.tts_voice,
    };
    ui_notify(CAP_VOICE_UI_SPEAKING);
    esp_err_t err = cap_voice_tts_speak(&tcfg, text);
    ui_notify(CAP_VOICE_UI_IDLE);
    cJSON_Delete(args);

    if (err == ESP_OK) {
        snprintf(output, output_size, "{\"spoken\":true}");
    } else {
        snprintf(output, output_size, "{\"error\":\"TTS failed\"}");
    }
    return ESP_OK;
}

/* LLM tool: voice_set_voice */
static esp_err_t voice_set_voice_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    (void)ctx;

    if (!output || output_size == 0) {
        cJSON_Delete(args);
        return ESP_ERR_INVALID_ARG;
    }

    const char *voice = cJSON_GetStringValue(cJSON_GetObjectItem(args, "voice"));
    if (voice) {
        strlcpy(s_session_voice, voice, sizeof(s_session_voice));
        s_config.tts_voice = s_session_voice;
        snprintf(output, output_size, "{\"ok\":true,\"voice\":\"%s\"}", voice);
    } else {
        snprintf(output, output_size, "{\"error\":\"missing voice\"}");
    }
    cJSON_Delete(args);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_voice_descriptors[] = {
    {
        .id          = "voice_say",
        .name        = "voice_say",
        .family      = "voice",
        .description = "Speak text aloud through the device speaker via TTS.",
        .kind        = CLAW_CAP_KIND_CALLABLE,
        .cap_flags   = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},"
            "\"required\":[\"text\"]}",
        .execute     = voice_say_execute,
    },
    {
        .id          = "voice_set_voice",
        .name        = "voice_set_voice",
        .family      = "voice",
        .description = "Change TTS voice for this session.",
        .kind        = CLAW_CAP_KIND_CALLABLE,
        .cap_flags   = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"voice\":{\"type\":\"string\","
            "\"enum\":[\"alloy\",\"echo\",\"fable\",\"onyx\",\"nova\",\"shimmer\"]}},"
            "\"required\":[\"voice\"]}",
        .execute     = voice_set_voice_execute,
    },
};

static const claw_cap_group_t s_voice_group = {
    .group_id        = "cap_voice",
    .plugin_name     = "cap_voice",
    .descriptors     = s_voice_descriptors,
    .descriptor_count = sizeof(s_voice_descriptors) / sizeof(s_voice_descriptors[0]),
};

esp_err_t cap_voice_register_group(void)
{
    return claw_cap_register_group(&s_voice_group);
}

esp_err_t cap_voice_set_config(const cap_voice_config_t *config)
{
    s_config = *config;
    return ESP_OK;
}

esp_err_t cap_voice_start(void)
{
    ESP_RETURN_ON_ERROR(cap_voice_audio_init(), TAG, "audio init failed");

    cap_voice_pipeline_config_t pcfg = {
        .on_wakeup       = on_wakeup,
        .on_command      = on_command,
        .on_transcript   = on_transcript,
        .cb_ctx          = NULL,
        .wake_sensitivity    = s_config.wake_sensitivity > 0
                               ? s_config.wake_sensitivity : 0.7f,
        .multinet_threshold  = s_config.multinet_threshold > 0
                               ? s_config.multinet_threshold : 0.85f,
        .whisper_api_key = s_config.whisper_api_key,
        .whisper_base_url = s_config.whisper_base_url,
        .tts_api_key     = s_config.tts_api_key,
        .tts_base_url    = s_config.tts_base_url,
    };
    ESP_RETURN_ON_ERROR(cap_voice_pipeline_init(&pcfg), TAG, "pipeline init failed");
    return cap_voice_pipeline_start();
}

esp_err_t cap_voice_stop(void)
{
    cap_voice_pipeline_stop();
    return cap_voice_audio_deinit();
}

esp_err_t cap_voice_speak(const char *text)
{
    if (!text) return ESP_ERR_INVALID_ARG;
    cap_voice_tts_config_t tcfg = {
        .api_key  = s_config.tts_api_key,
        .base_url = s_config.tts_base_url,
        .voice    = s_config.tts_voice,
    };
    return cap_voice_tts_speak(&tcfg, text);
}
