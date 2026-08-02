/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_rover_s3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_capabilities.h"
#include "app_claw.h"
#include "cap_rover.h"
#include "cap_unitv.h"
#include "cap_voice.h"
#include "esp_check.h"
#include "esp_log.h"
#include "rover_s3_cli.h"
#include "rover_s3_display.h"
#include "rover_s3_httpd.h"
#include "rover_s3_settings.h"
#include "rover_s3_wifi.h"
#include "setup_device.h"

static const char *TAG = "app_rover_s3";

#define ROVER_S3_SYSTEM_PROMPT \
    "You are AI Rover S3, a mecanum robot with a gripper, a camera, and a speaker. " \
    "Answer briefly in the user's language. " \
    "Use rover_move for movement, rover_turn for rotation, " \
    "unitv_scan for quick detection, unitv_capture for scene analysis. " \
    "Use voice_say to speak responses aloud when the user communicates via voice. " \
    "For multi-step tasks, activate rover_ops or rover_search first."

rover_s3_settings_t g_settings;

static void wifi_state_cb(bool connected, void *user_ctx)
{
    (void)user_ctx;
    rover_s3_display_set_state(connected ? ROVER_S3_DISPLAY_IDLE : ROVER_S3_DISPLAY_OFFLINE);
    if (connected) {
        rover_s3_httpd_start();
    } else {
        rover_s3_httpd_stop();
    }
}

static void voice_ui_cb(cap_voice_ui_state_t state, void *ctx)
{
    (void)ctx;
    switch (state) {
    case CAP_VOICE_UI_LISTENING: rover_s3_display_set_state(ROVER_S3_DISPLAY_LISTENING);  break;
    case CAP_VOICE_UI_THINKING:  rover_s3_display_set_state(ROVER_S3_DISPLAY_THINKING);   break;
    case CAP_VOICE_UI_SPEAKING:  rover_s3_display_set_state(ROVER_S3_DISPLAY_SPEAKING);   break;
    case CAP_VOICE_UI_IDLE:      rover_s3_display_set_state(ROVER_S3_DISPLAY_IDLE);       break;
    }
}

/* --- external capability groups registered with app_claw's generic bootstrap ---
 * cap_rover / cap_unitv / cap_voice are rover_s3-specific hardware
 * capabilities with no generic app_claw counterpart. Each "prepare" callback
 * runs the board-specific hardware init (I2C/UART bring-up); each "reg"
 * callback registers the capability's tools with claw_cap and, where the
 * original app also wired a CLI command set, does that too. app_claw calls
 * prepare() then reg() for each enabled group before starting claw_cap. */

static esp_err_t app_cap_prepare_rover(const app_claw_config_t *config,
                                       const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_rover_init(&(cap_rover_config_t){
                              .i2c_port = 1,   /* external I2C / Grove: GPIO 9/10 */
                              .sda_gpio = 9,
                              .scl_gpio = 10,
                              .i2c_freq_hz = 100000,
                              .rover_addr = 0x38,
                              .gripper_servo_idx = 1,
                              .gripper_open_angle = 35,
                              .gripper_close_angle = 150,
                              .hw_task_stack_size = 4096,
                              .hw_task_priority = 5,
                              .hw_task_core = 0,
                          });
}

static esp_err_t app_cap_register_rover(const app_claw_config_t *config,
                                        const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    ESP_RETURN_ON_ERROR(cap_rover_register_group(), TAG, "register rover");
    cap_rover_register_cli();
    return ESP_OK;
}

static esp_err_t app_cap_prepare_unitv(const app_claw_config_t *config,
                                       const app_claw_storage_paths_t *paths)
{
    (void)paths;
    ESP_RETURN_ON_ERROR(cap_unitv_init(&(cap_unitv_config_t){
                            .uart_port = 1,
                            .tx_gpio = 5,
                            .rx_gpio = 6,
                            .baud_rate = 115200,
                            .rx_buffer_bytes = 4096,
                            .default_timeout_ms = 7000,
                            .capture_timeout_ms = 12000,
                            .max_jpeg_bytes = 6144,
                        }),
                        TAG, "unitv init");
    cap_unitv_set_vision_config(&(cap_unitv_vision_config_t){
        .api_key = config->llm_api_key,
        .backend_type = config->llm_backend_type,
        .model = config->llm_model,
        .base_url = config->llm_base_url,
        .auth_type = config->llm_auth_type,
        .timeout_ms = (uint32_t)strtoul(config->llm_timeout_ms, NULL, 10),
        .max_response_tokens = 256,
    });
    return ESP_OK;
}

static esp_err_t app_cap_register_unitv(const app_claw_config_t *config,
                                        const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    ESP_RETURN_ON_ERROR(cap_unitv_register_group(), TAG, "register unitv");
    cap_unitv_register_cli();
    return ESP_OK;
}

static esp_err_t app_cap_prepare_voice(const app_claw_config_t *config,
                                       const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    cap_voice_config_t vcfg = {
        .whisper_api_key  = g_settings.whisper_api_key[0]
                            ? g_settings.whisper_api_key : g_settings.llm_api_key,
        .whisper_base_url = g_settings.whisper_base_url[0]
                            ? g_settings.whisper_base_url : g_settings.llm_base_url,
        .tts_api_key      = g_settings.tts_api_key[0]
                            ? g_settings.tts_api_key : g_settings.llm_api_key,
        .tts_base_url     = g_settings.tts_base_url[0]
                            ? g_settings.tts_base_url : g_settings.llm_base_url,
        .tts_voice        = g_settings.tts_voice[0] ? g_settings.tts_voice : "alloy",
        .wake_sensitivity = atof(g_settings.wake_sensitivity[0]
                                 ? g_settings.wake_sensitivity : "0.7"),
        .multinet_threshold = atof(g_settings.multinet_threshold[0]
                                   ? g_settings.multinet_threshold : "0.85"),
        .on_ui_state = voice_ui_cb,
        .ui_ctx      = NULL,
    };
    cap_voice_set_config(&vcfg);
    return ESP_OK;
}

static esp_err_t app_cap_register_voice(const app_claw_config_t *config,
                                        const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_voice_register_group();
}

static esp_err_t register_rover_s3_capabilities(void)
{
    ESP_RETURN_ON_ERROR(app_capabilities_register_external_group(&(app_capability_external_group_t){
                            .group_id = "cap_rover",
                            .display_name = "Rover",
                            .llm_visible_by_default = true,
                            .prepare = app_cap_prepare_rover,
                            .reg = app_cap_register_rover,
                        }),
                        TAG, "register external group cap_rover");
    ESP_RETURN_ON_ERROR(app_capabilities_register_external_group(&(app_capability_external_group_t){
                            .group_id = "cap_unitv",
                            .display_name = "UnitV",
                            .llm_visible_by_default = true,
                            .prepare = app_cap_prepare_unitv,
                            .reg = app_cap_register_unitv,
                        }),
                        TAG, "register external group cap_unitv");
    ESP_RETURN_ON_ERROR(app_capabilities_register_external_group(&(app_capability_external_group_t){
                            .group_id = "cap_voice",
                            .display_name = "Voice",
                            .llm_visible_by_default = true,
                            .prepare = app_cap_prepare_voice,
                            .reg = app_cap_register_voice,
                        }),
                        TAG, "register external group cap_voice");
    return ESP_OK;
}

static bool llm_is_configured(const rover_s3_settings_t *s)
{
    return s && s->llm_api_key[0] && s->llm_model[0] && s->llm_profile[0];
}

static void build_app_claw_config(const rover_s3_settings_t *s, app_claw_config_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->llm_api_key, s->llm_api_key, sizeof(out->llm_api_key));
    strlcpy(out->llm_backend_type, s->llm_backend_type, sizeof(out->llm_backend_type));
    strlcpy(out->llm_model, s->llm_model, sizeof(out->llm_model));
    strlcpy(out->llm_base_url, s->llm_base_url, sizeof(out->llm_base_url));
    strlcpy(out->llm_auth_type, s->llm_auth_type, sizeof(out->llm_auth_type));
    strlcpy(out->llm_timeout_ms, s->llm_timeout_ms, sizeof(out->llm_timeout_ms));
    strlcpy(out->llm_max_tokens, "0", sizeof(out->llm_max_tokens));
    strlcpy(out->llm_default_image_max_bytes, "0", sizeof(out->llm_default_image_max_bytes));
    strlcpy(out->llm_supports_tools, "1", sizeof(out->llm_supports_tools));
    strlcpy(out->llm_supports_vision, "0", sizeof(out->llm_supports_vision));
    strlcpy(out->llm_image_remote_url_only, "0", sizeof(out->llm_image_remote_url_only));
    strlcpy(out->tg_bot_token, s->tg_bot_token, sizeof(out->tg_bot_token));
    /* Empty enabled_cap_groups = enable every compiled-in group (Kconfig
     * already trims the built-in set to what rover_s3 needs). */
    out->enabled_cap_groups[0] = '\0';
    strlcpy(out->llm_visible_cap_groups, "cap_rover,cap_unitv,cap_skill,cap_voice",
           sizeof(out->llm_visible_cap_groups));
    out->system_prompt_override = ROVER_S3_SYSTEM_PROMPT;
}

esp_err_t app_rover_s3_start(void)
{
    ESP_RETURN_ON_ERROR(rover_s3_settings_init(), TAG, "settings init");
    ESP_RETURN_ON_ERROR(rover_s3_settings_load(&g_settings), TAG, "settings load");
    ESP_RETURN_ON_ERROR(rover_s3_wifi_register_state_cb(wifi_state_cb, NULL), TAG, "wifi callback");

    ESP_RETURN_ON_ERROR(rover_s3_cli_init(), TAG, "console");
    ESP_RETURN_ON_ERROR(register_rover_s3_capabilities(), TAG, "register rover_s3 capabilities");

    app_claw_config_t claw_cfg;
    build_app_claw_config(&g_settings, &claw_cfg);
    ESP_RETURN_ON_ERROR(app_claw_start(&claw_cfg), TAG, "app_claw start");

    ESP_RETURN_ON_ERROR(rover_s3_cli_start(), TAG, "console REPL");

    if (strcmp(g_settings.voice_enabled, "0") != 0) {
        esp_err_t voice_err = cap_voice_start();
        if (voice_err == ESP_OK) {
            ESP_LOGI(TAG, "Voice pipeline started");
        } else {
            ESP_LOGW(TAG, "Voice pipeline start failed (%s); continuing without voice",
                     esp_err_to_name(voice_err));
        }
    }

    if (!llm_is_configured(&g_settings)) {
        ESP_LOGW(TAG, "LLM not configured; agent routing is disabled");
    }

    rover_s3_display_set_state(rover_s3_wifi_is_connected()
                                ? ROVER_S3_DISPLAY_IDLE : ROVER_S3_DISPLAY_OFFLINE);

    /* WiFi may have connected before the state callback was registered — start httpd now */
    if (rover_s3_wifi_is_connected()) {
        rover_s3_httpd_start();
    }

    return ESP_OK;
}
