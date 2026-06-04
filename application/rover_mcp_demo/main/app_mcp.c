/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_mcp.h"

#include <stdlib.h>

#include "cap_mcp_server.h"
#include "cap_rover.h"
#include "cap_session_mgr.h"
#include "cap_unitv.h"
#include "claw_cap.h"
#include "claw_event_router.h"
#include "cmd_cap_mcp_server.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rover_buttons.h"
#include "rover_demo_cli.h"
#include "rover_demo_wifi.h"
#include "rover_display.h"
#include "rover_mcp_tools.h"
#include "setup_device.h"

static const char *TAG = "app_mcp";

static esp_err_t imu_read_adapter(float *ax, float *ay, float *az,
                                  float *gx, float *gy, float *gz)
{
    return rover_board_imu_read(ax, ay, az, gx, gy, gz);
}

static esp_err_t power_read_adapter(int *battery_pct, bool *charging, int *battery_mv)
{
    rover_power_status_t ps = {0};
    rover_board_get_power_status(&ps);
    if (battery_pct) {
        *battery_pct = ps.battery_pct;
    }
    if (charging) {
        *charging = ps.charging;
    }
    if (battery_mv) {
        *battery_mv = ps.battery_mv;
    }
    return ESP_OK;
}

static void deep_sleep_cb(void *user_ctx)
{
    (void)user_ctx;
    rover_display_set_state(ROVER_DISPLAY_STATE_SLEEPING);
    rover_display_refresh();
    vTaskDelay(pdMS_TO_TICKS(100));
    rover_board_display_sleep();
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_37, 0);
    esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_39, ESP_EXT1_WAKEUP_ALL_LOW);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_deep_sleep_start();
}

static void wifi_state_cb(bool connected, void *user_ctx)
{
    (void)user_ctx;
    rover_display_set_state(connected ? ROVER_DISPLAY_STATE_IDLE : ROVER_DISPLAY_STATE_OFFLINE);
}

esp_err_t app_mcp_start(const rover_demo_settings_t *s)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(rover_demo_wifi_register_state_cb(wifi_state_cb, NULL), TAG, "wifi cb");

    /* Minimal event router — no rules, no agent. Needed for MCP server's emit_event tool. */
    char rules_path[80];
    snprintf(rules_path, sizeof(rules_path), "%s/router_rules/router_rules.json",
             rover_demo_fatfs_base_path);
    claw_event_router_config_t router_cfg = {
        .rules_path = rules_path,
        .task_stack_size = 4 * 1024,
        .task_priority = 5,
        .task_core = tskNO_AFFINITY,
        .core_submit_timeout_ms = 1000,
        .core_receive_timeout_ms = 10000,
        .default_route_messages_to_agent = false,
        .session_builder = cap_session_mgr_build_session_id,
    };
    ESP_RETURN_ON_ERROR(cap_session_mgr_set_session_root_dir("/fatfs/sessions"), TAG, "session root");
    ESP_RETURN_ON_ERROR(claw_event_router_init(&router_cfg), TAG, "event router");

    /* Register rover MCP tools before MCP server init. */
    ESP_RETURN_ON_ERROR(cap_mcp_server_set_config(&(cap_mcp_server_config_t){
                            .hostname = "ai-rover",
                            .instance_name = "AI Rover MCP",
                        }),
                        TAG, "mcp config");
    ESP_RETURN_ON_ERROR(rover_mcp_tools_register(), TAG, "rover mcp tools");

    ESP_RETURN_ON_ERROR(claw_cap_init(), TAG, "cap init");
    ESP_RETURN_ON_ERROR(cap_rover_init(&(cap_rover_config_t){
                            .i2c_port = 0,
                            .sda_gpio = 0,
                            .scl_gpio = 26,
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
    cap_rover_set_imu_read(imu_read_adapter);
    cap_rover_set_power_read(power_read_adapter);

    ESP_RETURN_ON_ERROR(cap_unitv_init(&(cap_unitv_config_t){
                            .uart_port = 1,
                            .tx_gpio = 32,
                            .rx_gpio = 33,
                            .baud_rate = 115200,
                            .rx_buffer_bytes = 4096,
                            .default_timeout_ms = 7000,
                            .capture_timeout_ms = 12000,
                            .max_jpeg_bytes = 6144,
                        }),
                        TAG, "unitv init");
    if (s->llm_api_key[0] && s->llm_model[0]) {
        cap_unitv_set_vision_config(&(cap_unitv_vision_config_t){
            .api_key = s->llm_api_key,
            .backend_type = s->llm_backend_type,
            .model = s->llm_model,
            .base_url = s->llm_base_url,
            .auth_type = s->llm_auth_type,
            .timeout_ms = (uint32_t)strtoul(s->llm_timeout_ms, NULL, 10),
            .max_response_tokens = 256,
        });
    }

    ESP_RETURN_ON_ERROR(cap_rover_register_group(), TAG, "register rover");
    ESP_RETURN_ON_ERROR(cap_unitv_register_group(), TAG, "register unitv");
    ESP_RETURN_ON_ERROR(cap_session_mgr_register_group(), TAG, "register sessions");
    ESP_RETURN_ON_ERROR(cap_mcp_server_register_group(), TAG, "register mcp server");
    ESP_RETURN_ON_ERROR(claw_cap_start_all(), TAG, "start caps");

    ESP_RETURN_ON_ERROR(rover_demo_cli_init(), TAG, "console");
    cap_rover_register_cli();
    cap_unitv_register_cli();
    register_cap_mcp_server();
    ESP_RETURN_ON_ERROR(rover_demo_cli_start(), TAG, "console REPL");

    ESP_RETURN_ON_ERROR(claw_event_router_start(), TAG, "router start");
    ESP_RETURN_ON_ERROR(rover_buttons_init(&(rover_buttons_config_t){
                            .on_btn_a_long = deep_sleep_cb,
                        }),
                        TAG, "buttons");

    rover_display_set_state(rover_demo_wifi_is_connected() ? ROVER_DISPLAY_STATE_IDLE : ROVER_DISPLAY_STATE_OFFLINE);
    ESP_LOGI(TAG, "MCP server ready at http://ai-rover.local:18791/mcp_server");
    return ESP_OK;
}
