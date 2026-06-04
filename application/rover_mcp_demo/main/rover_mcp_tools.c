/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_mcp_tools.h"

#include <stdio.h>
#include <string.h>

#include "cap_mcp_server.h"
#include "claw_cap.h"
#include "esp_mcp_data.h"
#include "esp_mcp_property.h"

#define TOOL_OUT_SIZE 512

static esp_mcp_value_t call_cap(const char *cap_id, const char *input_json)
{
    char out[TOOL_OUT_SIZE] = {0};
    esp_err_t err = claw_cap_call(cap_id, input_json, NULL, out, sizeof(out));
    if (err != ESP_OK || !out[0]) {
        snprintf(out, sizeof(out), "{\"status\":\"error\",\"cap\":\"%s\"}", cap_id);
    }
    return esp_mcp_value_create_string(out);
}

static esp_mcp_value_t rover_move_cb(const esp_mcp_property_list_t *p)
{
    int x = esp_mcp_property_list_get_property_int(p, "x");
    int y = esp_mcp_property_list_get_property_int(p, "y");
    int z = esp_mcp_property_list_get_property_int(p, "z");
    int dur = esp_mcp_property_list_get_property_int(p, "duration_ms");
    if (!dur) {
        dur = 1500;
    }
    char input[96];
    snprintf(input, sizeof(input),
             "{\"x\":%d,\"y\":%d,\"z\":%d,\"duration_ms\":%d}", x, y, z, dur);
    return call_cap("rover_move", input);
}

static esp_mcp_value_t rover_turn_cb(const esp_mcp_property_list_t *p)
{
    const char *dir = esp_mcp_property_list_get_property_string(p, "direction");
    int angle = esp_mcp_property_list_get_property_int(p, "angle_deg");
    int speed = esp_mcp_property_list_get_property_int(p, "speed_percent");
    char input[96];
    snprintf(input, sizeof(input),
             "{\"direction\":\"%s\",\"angle_deg\":%d,\"speed_percent\":%d}",
             dir ? dir : "left", angle ? angle : 90, speed ? speed : 50);
    return call_cap("rover_turn", input);
}

static esp_mcp_value_t rover_stop_cb(const esp_mcp_property_list_t *p)
{
    (void)p;
    return call_cap("rover_stop", "{}");
}

static esp_mcp_value_t rover_gripper_open_cb(const esp_mcp_property_list_t *p)
{
    (void)p;
    return call_cap("rover_gripper_open", "{}");
}

static esp_mcp_value_t rover_gripper_close_cb(const esp_mcp_property_list_t *p)
{
    (void)p;
    return call_cap("rover_gripper_close", "{}");
}

static esp_mcp_value_t rover_imu_cb(const esp_mcp_property_list_t *p)
{
    (void)p;
    return call_cap("rover_read_imu", "{}");
}

static esp_mcp_value_t rover_power_cb(const esp_mcp_property_list_t *p)
{
    (void)p;
    return call_cap("rover_power_status", "{}");
}

static esp_mcp_value_t rover_scan_cb(const esp_mcp_property_list_t *p)
{
    (void)p;
    return call_cap("unitv_scan", "{}");
}

static esp_mcp_value_t rover_capture_cb(const esp_mcp_property_list_t *p)
{
    const char *q = esp_mcp_property_list_get_property_string(p, "question");
    char input[320];
    if (q && q[0]) {
        snprintf(input, sizeof(input), "{\"question\":\"%s\",\"quality\":30}", q);
    } else {
        snprintf(input, sizeof(input), "{\"quality\":30}");
    }
    return call_cap("unitv_capture", input);
}

esp_err_t rover_mcp_tools_register(void)
{
    static const cap_mcp_server_extra_tool_t tools[] = {
        {
            .name = "rover.move",
            .description = "Move rover. x/y/z: velocity -100..100 (x=forward, y=strafe, z=rotation). duration_ms: 100-10000.",
            .callback = rover_move_cb,
            .property_names = {"x", "y", "z", "duration_ms"},
            .property_count = 4,
        },
        {
            .name = "rover.turn",
            .description = "Turn rover in place using IMU. direction: left|right. angle_deg: 5-360. speed_percent: 20-100.",
            .callback = rover_turn_cb,
            .property_names = {"direction", "angle_deg", "speed_percent"},
            .property_count = 3,
        },
        {
            .name = "rover.stop",
            .description = "Emergency stop all rover motion.",
            .callback = rover_stop_cb,
            .property_count = 0,
        },
        {
            .name = "rover.gripper_open",
            .description = "Open the rover gripper.",
            .callback = rover_gripper_open_cb,
            .property_count = 0,
        },
        {
            .name = "rover.gripper_close",
            .description = "Close the rover gripper.",
            .callback = rover_gripper_close_cb,
            .property_count = 0,
        },
        {
            .name = "rover.imu",
            .description = "Read rover IMU accelerometer and gyroscope.",
            .callback = rover_imu_cb,
            .property_count = 0,
        },
        {
            .name = "rover.power",
            .description = "Read battery level (%), charging state, and voltage (mV).",
            .callback = rover_power_cb,
            .property_count = 0,
        },
        {
            .name = "rover.scan",
            .description = "Run UnitV onboard object detection scan.",
            .callback = rover_scan_cb,
            .property_count = 0,
        },
        {
            .name = "rover.capture",
            .description = "Capture JPEG from rover camera and analyze with vision LLM. Provide a question.",
            .callback = rover_capture_cb,
            .property_names = {"question"},
            .property_count = 1,
        },
    };

    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        esp_err_t err = cap_mcp_server_add_tool(&tools[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
