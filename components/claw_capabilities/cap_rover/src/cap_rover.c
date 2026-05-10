/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_rover_internal.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

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

static const char *err_to_status(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "ok";
    case ESP_ERR_INVALID_STATE:
        return "emergency_stop";
    case ESP_ERR_TIMEOUT:
        return "timeout";
    case ESP_ERR_NOT_SUPPORTED:
        return "imu_unavailable";
    default:
        return "failed";
    }
}

static esp_err_t cap_rover_move_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    int x = 0;
    int y = 0;
    int z = 0;
    int duration_ms = 1500;
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (args) {
        cJSON *v = cJSON_GetObjectItem(args, "x");
        if (cJSON_IsNumber(v)) {
            x = v->valueint;
        }
        v = cJSON_GetObjectItem(args, "y");
        if (cJSON_IsNumber(v)) {
            y = v->valueint;
        }
        v = cJSON_GetObjectItem(args, "z");
        if (cJSON_IsNumber(v)) {
            z = v->valueint;
        }
        v = cJSON_GetObjectItem(args, "duration_ms");
        if (cJSON_IsNumber(v)) {
            duration_ms = v->valueint;
        }
        cJSON_Delete(args);
    }

    x = clamp_int(x, -100, 100);
    y = clamp_int(y, -100, 100);
    z = clamp_int(z, -100, 100);
    duration_ms = clamp_int(duration_ms, 100, 5000);

    rover_action_req_t req = {
        .kind = ROVER_ACTION_MOVE,
        .x = (int8_t)x,
        .y = (int8_t)y,
        .z = (int8_t)z,
        .duration_ms = (uint16_t)duration_ms,
    };
    rover_action_result_t result = {0};
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(duration_ms + 1000), &result);

    snprintf(output, output_size,
             "{\"status\":\"%s\",\"action\":\"rover_move\",\"x\":%d,\"y\":%d,\"z\":%d,\"duration_ms\":%d}",
             err_to_status(err), x, y, z, duration_ms);
    return ESP_OK;
}

static esp_err_t cap_rover_turn_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    char dir[8] = "left";
    int angle_deg = 90;
    int speed_pct = 50;
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (args) {
        cJSON *v = cJSON_GetObjectItem(args, "direction");
        if (cJSON_IsString(v) && v->valuestring) {
            strlcpy(dir, v->valuestring, sizeof(dir));
        }
        v = cJSON_GetObjectItem(args, "angle_deg");
        if (cJSON_IsNumber(v)) {
            angle_deg = v->valueint;
        }
        v = cJSON_GetObjectItem(args, "speed_percent");
        if (cJSON_IsNumber(v)) {
            speed_pct = v->valueint;
        }
        cJSON_Delete(args);
    }

    bool turn_left = strcmp(dir, "right") != 0;
    int target = clamp_int(angle_deg, 5, 360);
    int spd = clamp_int(speed_pct, 20, 100);
    uint32_t timeout_ms = (uint32_t)clamp_int(target * 100, 2000, 12000);

    rover_action_req_t req = {
        .kind = ROVER_ACTION_TURN,
        .z = (int8_t)(turn_left ? -spd : spd),
        .turn_target_deg = (uint16_t)target,
        .turn_timeout_ms = (uint16_t)timeout_ms,
    };
    rover_action_result_t result = {0};
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(timeout_ms + 1000), &result);

    snprintf(output, output_size,
             "{\"status\":\"%s\",\"action\":\"rover_turn\",\"direction\":\"%s\","
             "\"target_deg\":%d,\"measured_deg\":%.1f}",
             err_to_status(err), turn_left ? "left" : "right",
             target, (double)result.turn_measured_deg);
    return ESP_OK;
}

static esp_err_t cap_rover_stop_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    rover_action_req_t req = { .kind = ROVER_ACTION_STOP };
    rover_action_result_t result = {0};
    (void)input_json;
    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(2000), &result);
    snprintf(output, output_size, "{\"status\":\"%s\",\"action\":\"rover_stop\"}", err_to_status(err));
    return ESP_OK;
}

static esp_err_t gripper_execute(rover_action_kind_t kind,
                                 const char *action_label,
                                 char *output,
                                 size_t output_size)
{
    rover_action_req_t req = { .kind = kind };
    rover_action_result_t result = {0};
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(2000), &result);

    snprintf(output, output_size,
             "{\"status\":\"%s\",\"action\":\"%s\"}",
             err_to_status(err), action_label);
    return ESP_OK;
}

static esp_err_t cap_rover_gripper_open_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    (void)input_json;
    (void)ctx;
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return gripper_execute(ROVER_ACTION_GRIPPER_OPEN, "rover_gripper_open", output, output_size);
}

static esp_err_t cap_rover_gripper_close_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size)
{
    (void)input_json;
    (void)ctx;
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return gripper_execute(ROVER_ACTION_GRIPPER_CLOSE, "rover_gripper_close", output, output_size);
}

static esp_err_t cap_rover_read_imu_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    float gx = 0.0f;
    float gy = 0.0f;
    float gz = 0.0f;
    (void)input_json;
    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_cap_rover.imu_read) {
        snprintf(output, output_size, "{\"status\":\"imu_unavailable\",\"action\":\"rover_read_imu\"}");
        return ESP_OK;
    }
    if (g_cap_rover.imu_read(&ax, &ay, &az, &gx, &gy, &gz) != ESP_OK) {
        snprintf(output, output_size, "{\"status\":\"imu_read_failed\",\"action\":\"rover_read_imu\"}");
        return ESP_OK;
    }

    snprintf(output, output_size,
             "{\"status\":\"ok\",\"action\":\"rover_read_imu\","
             "\"accel\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
             "\"gyro\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}",
             (double)ax, (double)ay, (double)az,
             (double)gx, (double)gy, (double)gz);
    return ESP_OK;
}

static esp_err_t cap_rover_power_status_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    (void)input_json;
    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_cap_rover.power_read) {
        snprintf(output, output_size, "{\"status\":\"unavailable\",\"action\":\"rover_power_status\"}");
        return ESP_OK;
    }

    int battery_pct = -1;
    bool charging = false;
    int battery_mv = 0;
    g_cap_rover.power_read(&battery_pct, &charging, &battery_mv);

    snprintf(output, output_size,
             "{\"status\":\"ok\",\"action\":\"rover_power_status\","
             "\"battery_pct\":%d,\"charging\":%s,\"battery_mv\":%d}",
             battery_pct, charging ? "true" : "false", battery_mv);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_rover_descriptors[] = {
    {
        .id = "rover_move",
        .name = "rover_move",
        .family = "rover",
        .description = "Move rover: x/y/z velocity (-100..100), duration_ms.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"x\":{\"type\":\"integer\",\"minimum\":-100,\"maximum\":100},"
            "\"y\":{\"type\":\"integer\",\"minimum\":-100,\"maximum\":100},"
            "\"z\":{\"type\":\"integer\",\"minimum\":-100,\"maximum\":100},"
            "\"duration_ms\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":5000}"
            "},\"required\":[\"x\",\"y\"]}",
        .execute = cap_rover_move_execute,
    },
    {
        .id = "rover_turn",
        .name = "rover_turn",
        .family = "rover",
        .description = "Rotate the rover in place by angle using IMU gyro feedback.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"direction\":{\"type\":\"string\",\"enum\":[\"left\",\"right\"]},"
            "\"angle_deg\":{\"type\":\"integer\",\"minimum\":5,\"maximum\":360},"
            "\"speed_percent\":{\"type\":\"integer\",\"minimum\":20,\"maximum\":100}"
            "},\"required\":[\"direction\"]}",
        .execute = cap_rover_turn_execute,
    },
    {
        .id = "rover_stop",
        .name = "rover_stop",
        .family = "rover",
        .description = "Stop all rover motion immediately.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_rover_stop_execute,
    },
    {
        .id = "rover_gripper_open",
        .name = "rover_gripper_open",
        .family = "rover",
        .description = "Open the rover gripper.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_rover_gripper_open_execute,
    },
    {
        .id = "rover_gripper_close",
        .name = "rover_gripper_close",
        .family = "rover",
        .description = "Close the rover gripper.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_rover_gripper_close_execute,
    },
    {
        .id = "rover_read_imu",
        .name = "rover_read_imu",
        .family = "rover",
        .description = "Read rover IMU (accelerometer + gyroscope).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_rover_read_imu_execute,
    },
    {
        .id = "rover_power_status",
        .name = "rover_power_status",
        .family = "rover",
        .description = "Read battery level (%), charging state, and voltage (mV).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_rover_power_status_execute,
    },
};

static const claw_cap_group_t s_rover_group = {
    .group_id = "cap_rover",
    .plugin_name = "cap_rover",
    .descriptors = s_rover_descriptors,
    .descriptor_count = sizeof(s_rover_descriptors) / sizeof(s_rover_descriptors[0]),
};

esp_err_t cap_rover_register_group(void)
{
    if (claw_cap_group_exists(s_rover_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_rover_group);
}
