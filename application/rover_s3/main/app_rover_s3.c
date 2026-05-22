/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_rover_s3.h"

#include <stdio.h>
#include <stdlib.h>

#include "cap_files.h"
#include "cap_im_tg.h"
#include "cap_rover.h"
#include "cap_session_mgr.h"
#include "cap_skill_mgr.h"
#include "cap_system.h"
#include "cap_time.h"
#include "cap_unitv.h"
#include "cap_voice.h"
#include "claw_cap.h"
#include "claw_core.h"
#include "claw_event_publisher.h"
#include "claw_event_router.h"
#include "claw_memory.h"
#include "claw_skill.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rover_s3_cli.h"
#include "rover_s3_display.h"
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

static const char *const ROVER_S3_LLM_VISIBLE_GROUPS[] = {
    "cap_rover",
    "cap_unitv",
    "cap_skill",
    "cap_voice",
};

rover_s3_settings_t g_settings;

typedef struct {
    char memory_session_root[64];
    char memory_root_dir[64];
    char skills_root_dir[64];
    char router_rules_path[96];
    char im_attachment_root[64];
} rover_paths_t;

static rover_paths_t s_paths;

static esp_err_t init_paths(rover_paths_t *p)
{
    const char *base = rover_s3_fatfs_base_path;

    if (!p || !base || base[0] != '/') {
        return ESP_ERR_INVALID_STATE;
    }
    if (snprintf(p->memory_session_root, sizeof(p->memory_session_root), "%s/sessions", base) >= sizeof(p->memory_session_root) ||
            snprintf(p->memory_root_dir, sizeof(p->memory_root_dir), "%s/memory", base) >= sizeof(p->memory_root_dir) ||
            snprintf(p->skills_root_dir, sizeof(p->skills_root_dir), "%s/skills", base) >= sizeof(p->skills_root_dir) ||
            snprintf(p->router_rules_path, sizeof(p->router_rules_path), "%s/router_rules/router_rules.json", base) >= sizeof(p->router_rules_path) ||
            snprintf(p->im_attachment_root, sizeof(p->im_attachment_root), "%s/inbox", base) >= sizeof(p->im_attachment_root)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool llm_is_configured(const rover_s3_settings_t *s)
{
    return s && s->llm_api_key[0] && s->llm_model[0] && s->llm_profile[0];
}

static void wifi_state_cb(bool connected, void *user_ctx)
{
    (void)user_ctx;
    rover_s3_display_set_state(connected ? ROVER_S3_DISPLAY_IDLE : ROVER_S3_DISPLAY_OFFLINE);
}

esp_err_t app_rover_s3_start(void)
{
    ESP_RETURN_ON_ERROR(rover_s3_settings_init(), TAG, "settings init");
    ESP_RETURN_ON_ERROR(rover_s3_settings_load(&g_settings), TAG, "settings load");
    ESP_RETURN_ON_ERROR(init_paths(&s_paths), TAG, "init paths");
    ESP_RETURN_ON_ERROR(rover_s3_wifi_register_state_cb(wifi_state_cb, NULL), TAG, "wifi callback");

    claw_event_router_config_t router_cfg = {
        .rules_path = s_paths.router_rules_path,
        .task_stack_size = 8 * 1024,
        .task_priority = 5,
        .task_core = tskNO_AFFINITY,
        .core_submit_timeout_ms = 1000,
        .core_receive_timeout_ms = 130000,
        .default_route_messages_to_agent = llm_is_configured(&g_settings),
        .session_builder = cap_session_mgr_build_session_id,
    };
    ESP_RETURN_ON_ERROR(cap_session_mgr_set_session_root_dir(s_paths.memory_session_root), TAG, "session root");
    ESP_RETURN_ON_ERROR(claw_event_router_init(&router_cfg), TAG, "event router");

    claw_memory_config_t mem_cfg = {
        .session_root_dir = s_paths.memory_session_root,
        .memory_root_dir = s_paths.memory_root_dir,
        .max_message_chars = 128,
        .llm = {
            .api_key = g_settings.llm_api_key,
            .backend_type = g_settings.llm_backend_type,
            .model = g_settings.llm_model,
            .base_url = g_settings.llm_base_url,
            .auth_type = g_settings.llm_auth_type,
            .timeout_ms = (uint32_t)strtoul(g_settings.llm_timeout_ms, NULL, 10),
            .image_max_bytes = 0,
        },
        .enable_async_extract_stage_note = false,
    };
    ESP_RETURN_ON_ERROR(claw_memory_init(&mem_cfg), TAG, "memory");
    ESP_RETURN_ON_ERROR(claw_skill_init(&(claw_skill_config_t){
                            .skills_root_dir = s_paths.skills_root_dir,
                            .session_state_root_dir = s_paths.memory_session_root,
                            .max_file_bytes = 10 * 1024,
                        }),
                        TAG, "skills");

    ESP_RETURN_ON_ERROR(claw_cap_init(), TAG, "cap init");
    ESP_RETURN_ON_ERROR(cap_files_set_base_dir(rover_s3_fatfs_base_path), TAG, "files base");
    ESP_RETURN_ON_ERROR(cap_rover_init(&(cap_rover_config_t){
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
                        }),
                        TAG, "rover init");
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
        .api_key = g_settings.llm_api_key,
        .backend_type = g_settings.llm_backend_type,
        .model = g_settings.llm_model,
        .base_url = g_settings.llm_base_url,
        .auth_type = g_settings.llm_auth_type,
        .timeout_ms = (uint32_t)strtoul(g_settings.llm_timeout_ms, NULL, 10),
        .max_response_tokens = 256,
    });

    if (g_settings.tg_bot_token[0]) {
        ESP_RETURN_ON_ERROR(cap_im_tg_set_token(g_settings.tg_bot_token), TAG, "Telegram token");
    }
    ESP_RETURN_ON_ERROR(cap_im_tg_set_attachment_config(&(cap_im_tg_attachment_config_t){
                            .storage_root_dir = s_paths.im_attachment_root,
                            .max_inbound_file_bytes = 1024 * 1024,
                            .enable_inbound_attachments = false,
                        }),
                        TAG, "Telegram attachments");

    ESP_RETURN_ON_ERROR(cap_rover_register_group(), TAG, "register rover");
    ESP_RETURN_ON_ERROR(cap_unitv_register_group(), TAG, "register unitv");
    ESP_RETURN_ON_ERROR(cap_im_tg_register_group(), TAG, "register tg");
    ESP_RETURN_ON_ERROR(cap_files_register_group(), TAG, "register files");
    ESP_RETURN_ON_ERROR(cap_skill_mgr_register_group(), TAG, "register skills");
    ESP_RETURN_ON_ERROR(cap_system_register_group(), TAG, "register system");
    ESP_RETURN_ON_ERROR(cap_time_register_group(), TAG, "register time");
    ESP_RETURN_ON_ERROR(cap_session_mgr_register_group(), TAG, "register sessions");
    ESP_RETURN_ON_ERROR(cap_voice_register_group(), TAG, "register voice");

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
    };
    cap_voice_set_config(&vcfg);

    ESP_RETURN_ON_ERROR(claw_cap_set_llm_visible_groups(
                            ROVER_S3_LLM_VISIBLE_GROUPS,
                            sizeof(ROVER_S3_LLM_VISIBLE_GROUPS) / sizeof(ROVER_S3_LLM_VISIBLE_GROUPS[0])),
                        TAG, "visible groups");
    ESP_RETURN_ON_ERROR(claw_cap_start_all(), TAG, "start caps");
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("telegram", "tg_send_message"), TAG, "tg outbound");

    ESP_RETURN_ON_ERROR(rover_s3_cli_init(), TAG, "console");
    cap_rover_register_cli();
    cap_unitv_register_cli();
    ESP_RETURN_ON_ERROR(rover_s3_cli_start(), TAG, "console REPL");

    if (strcmp(g_settings.voice_enabled, "0") != 0) {
        ESP_ERROR_CHECK(cap_voice_start());
        ESP_LOGI(TAG, "Voice pipeline started");
    }

    if (llm_is_configured(&g_settings)) {
        claw_core_config_t core_cfg = {
            .api_key = g_settings.llm_api_key,
            .backend_type = g_settings.llm_backend_type,
            .model = g_settings.llm_model,
            .base_url = g_settings.llm_base_url,
            .auth_type = g_settings.llm_auth_type,
            .timeout_ms = (uint32_t)strtoul(g_settings.llm_timeout_ms, NULL, 10),
            .system_prompt = ROVER_S3_SYSTEM_PROMPT,
            .persist_session = claw_memory_persist_session_callback,
            .call_cap = claw_cap_call_from_core,
            .task_stack_size = 16 * 1024,
            .task_priority = 5,
            .task_core = tskNO_AFFINITY,
            .max_tool_iterations = 20,
            .request_queue_len = 2,
            .response_queue_len = 2,
            .max_context_providers = 8,
        };
        ESP_RETURN_ON_ERROR(claw_core_init(&core_cfg), TAG, "core");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_profile_provider), TAG, "ctx profile");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_long_term_lightweight_provider), TAG, "ctx memory");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_skill_skills_list_provider), TAG, "ctx skills list");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_session_history_provider), TAG, "ctx history");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_cap_tools_provider), TAG, "ctx tools");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&cap_time_context_provider), TAG, "ctx time");
        ESP_RETURN_ON_ERROR(claw_core_start(), TAG, "core start");
    } else {
        ESP_LOGW(TAG, "LLM not configured; agent routing is disabled");
    }

    ESP_RETURN_ON_ERROR(claw_event_router_start(), TAG, "router start");

    rover_s3_display_set_state(rover_s3_wifi_is_connected()
                                ? ROVER_S3_DISPLAY_IDLE : ROVER_S3_DISPLAY_OFFLINE);
    return ESP_OK;
}
