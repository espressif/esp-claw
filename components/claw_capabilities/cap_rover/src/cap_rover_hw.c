/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_rover_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "cap_rover_hw";

cap_rover_state_t g_cap_rover = {0};

#define ROVER_REG_MOTOR_BASE        0x00
#define ROVER_REG_SERVO_ANGLE       0x10
#define ROVER_HW_TICK_MS            50
#define ROVER_GYRO_DEAD_BAND_DPS    3.0f
#define ROVER_RESULT_QUEUE_DEPTH    4
#define ROVER_REQ_QUEUE_DEPTH       4

static int8_t clamp_speed(int32_t v)
{
    if (v > 100) {
        return 100;
    }
    if (v < -100) {
        return -100;
    }
    return (int8_t)v;
}

static esp_err_t i2c_write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[8] = {0};

    if (!g_cap_rover.i2c_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_ARG;
    }

    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(g_cap_rover.i2c_dev, buf, len + 1, 50);
}

esp_err_t cap_rover_hw_set_speed(int8_t x, int8_t y, int8_t z)
{
    int32_t zn = -z;
    int32_t xa = x;
    int32_t ya = y;

    if (zn != 0) {
        int32_t scale = 100 - abs(zn);
        xa = (xa * scale) / 100;
        ya = (ya * scale) / 100;
    }

    int8_t buf[4] = {
        clamp_speed(ya + xa - zn),
        clamp_speed(ya - xa + zn),
        clamp_speed(ya - xa - zn),
        clamp_speed(ya + xa + zn),
    };

    return i2c_write_reg(ROVER_REG_MOTOR_BASE, (const uint8_t *)buf, sizeof(buf));
}

esp_err_t cap_rover_hw_set_servo_angle(uint8_t servo_idx, uint8_t angle)
{
    uint8_t reg = (uint8_t)(ROVER_REG_SERVO_ANGLE + servo_idx);
    return i2c_write_reg(reg, &angle, 1);
}

void cap_rover_hw_zero_motors(void)
{
    int8_t zero[4] = {0, 0, 0, 0};
    (void)i2c_write_reg(ROVER_REG_MOTOR_BASE, (const uint8_t *)zero, sizeof(zero));
}

void cap_rover_emergency_stop_set(void)
{
    g_cap_rover.emergency_requested = true;
}

void cap_rover_emergency_stop_clear(void)
{
    g_cap_rover.emergency_requested = false;
}

void cap_rover_set_imu_read(cap_rover_imu_read_fn fn)
{
    g_cap_rover.imu_read = fn;
}

static void send_result(uint32_t req_id, esp_err_t err, float measured_deg)
{
    rover_action_result_t r = {
        .req_id = req_id,
        .err = err,
        .turn_measured_deg = measured_deg,
    };

    if (xQueueSend(g_cap_rover.result_queue, &r, 0) != pdTRUE) {
        rover_action_result_t dropped = {0};
        (void)xQueueReceive(g_cap_rover.result_queue, &dropped, 0);
        (void)xQueueSend(g_cap_rover.result_queue, &r, 0);
    }
}

static esp_err_t execute_move(const rover_action_req_t *req, float *measured_deg)
{
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(req->duration_ms);
    esp_err_t err = ESP_OK;

    *measured_deg = 0.0f;
    while ((int32_t)(end - xTaskGetTickCount()) > 0) {
        if (g_cap_rover.emergency_requested) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        esp_err_t e = cap_rover_hw_set_speed(req->x, req->y, req->z);
        if (err == ESP_OK && e != ESP_OK) {
            err = e;
        }
        vTaskDelay(pdMS_TO_TICKS(ROVER_HW_TICK_MS));
    }

    cap_rover_hw_zero_motors();
    return err;
}

static esp_err_t execute_turn(const rover_action_req_t *req, float *measured_deg)
{
    TickType_t start_tick = xTaskGetTickCount();
    uint32_t prev_ms = esp_log_timestamp();
    esp_err_t err = ESP_OK;
    float turned = 0.0f;
    float target = (float)req->turn_target_deg;

    *measured_deg = 0.0f;
    if (!g_cap_rover.imu_read) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    while (turned < target &&
           (xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(req->turn_timeout_ms)) {
        float ax = 0.0f;
        float ay = 0.0f;
        float az = 0.0f;
        float gx = 0.0f;
        float gy = 0.0f;
        float gz = 0.0f;

        if (g_cap_rover.emergency_requested) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        if (g_cap_rover.imu_read(&ax, &ay, &az, &gx, &gy, &gz) != ESP_OK) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        uint32_t now_ms = esp_log_timestamp();
        float dt = (float)(now_ms - prev_ms) / 1000.0f;
        float rate = fabsf(gx);
        prev_ms = now_ms;

        if (fabsf(gy) > rate) {
            rate = fabsf(gy);
        }
        if (fabsf(gz) > rate) {
            rate = fabsf(gz);
        }
        if (rate > ROVER_GYRO_DEAD_BAND_DPS) {
            turned += rate * dt;
        }

        esp_err_t e = cap_rover_hw_set_speed(0, 0, req->z);
        if (err == ESP_OK && e != ESP_OK) {
            err = e;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    cap_rover_hw_zero_motors();
    *measured_deg = turned;
    if (err == ESP_OK && turned < target) {
        err = ESP_ERR_TIMEOUT;
    }
    return err;
}

static void hw_task(void *arg)
{
    rover_action_req_t req = {0};
    (void)arg;

    while (1) {
        if (xQueueReceive(g_cap_rover.req_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t err = ESP_OK;
        float measured_deg = 0.0f;

        switch (req.kind) {
        case ROVER_ACTION_MOVE:
            err = execute_move(&req, &measured_deg);
            break;
        case ROVER_ACTION_TURN:
            err = execute_turn(&req, &measured_deg);
            break;
        case ROVER_ACTION_STOP:
            cap_rover_hw_zero_motors();
            break;
        case ROVER_ACTION_GRIPPER_OPEN:
            err = cap_rover_hw_set_servo_angle(g_cap_rover.cfg.gripper_servo_idx,
                                               g_cap_rover.cfg.gripper_open_angle);
            break;
        case ROVER_ACTION_GRIPPER_CLOSE:
            err = cap_rover_hw_set_servo_angle(g_cap_rover.cfg.gripper_servo_idx,
                                               g_cap_rover.cfg.gripper_close_angle);
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
        }

        if (err == ESP_ERR_INVALID_STATE) {
            cap_rover_emergency_stop_clear();
        }
        send_result(req.req_id, err, measured_deg);
    }
}

esp_err_t cap_rover_hw_start(void)
{
    g_cap_rover.req_queue = xQueueCreate(ROVER_REQ_QUEUE_DEPTH, sizeof(rover_action_req_t));
    g_cap_rover.result_queue = xQueueCreate(ROVER_RESULT_QUEUE_DEPTH, sizeof(rover_action_result_t));
    g_cap_rover.queue_lock = xSemaphoreCreateMutex();
    if (!g_cap_rover.req_queue || !g_cap_rover.result_queue || !g_cap_rover.queue_lock) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(hw_task, "rover_hw",
                                            g_cap_rover.cfg.hw_task_stack_size,
                                            NULL,
                                            g_cap_rover.cfg.hw_task_priority,
                                            &g_cap_rover.hw_task,
                                            g_cap_rover.cfg.hw_task_core);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t cap_rover_submit_and_wait(rover_action_req_t *req,
                                    TickType_t timeout,
                                    rover_action_result_t *out_result)
{
    if (!req || !g_cap_rover.req_queue || !g_cap_rover.result_queue || !g_cap_rover.queue_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    req->req_id = (uint32_t)atomic_fetch_add(&g_cap_rover.req_seq, 1) + 1;

    xSemaphoreTake(g_cap_rover.queue_lock, portMAX_DELAY);
    if (xQueueSend(g_cap_rover.req_queue, req, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreGive(g_cap_rover.queue_lock);
        return ESP_ERR_TIMEOUT;
    }

    TickType_t deadline = xTaskGetTickCount() + timeout;
    esp_err_t final_err = ESP_ERR_TIMEOUT;
    while (1) {
        TickType_t now = xTaskGetTickCount();
        rover_action_result_t r = {0};

        if ((int32_t)(deadline - now) <= 0) {
            break;
        }
        if (xQueueReceive(g_cap_rover.result_queue, &r, deadline - now) != pdTRUE) {
            break;
        }
        if (r.req_id == req->req_id) {
            if (out_result) {
                *out_result = r;
            }
            final_err = r.err;
            break;
        }
    }

    xSemaphoreGive(g_cap_rover.queue_lock);
    return final_err;
}

esp_err_t cap_rover_init(const cap_rover_config_t *config)
{
    if (g_cap_rover.initialized) {
        return ESP_OK;
    }
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    g_cap_rover.cfg = *config;
    atomic_init(&g_cap_rover.req_seq, 0);
    g_cap_rover.emergency_requested = false;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = config->i2c_port,
        .sda_io_num = config->sda_gpio,
        .scl_io_num = config->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &g_cap_rover.i2c_bus),
                        TAG, "i2c_new_master_bus failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->rover_addr,
        .scl_speed_hz = config->i2c_freq_hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(g_cap_rover.i2c_bus, &dev_cfg, &g_cap_rover.i2c_dev),
                        TAG, "i2c_master_bus_add_device failed");

    esp_err_t e = cap_rover_hw_set_speed(0, 0, 0);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "Initial motor zero failed: %s", esp_err_to_name(e));
    }

    ESP_RETURN_ON_ERROR(cap_rover_hw_start(), TAG, "hw_start failed");
    g_cap_rover.initialized = true;
    ESP_LOGI(TAG, "initialized i2c_port=%d sda=%d scl=%d addr=0x%02x",
             config->i2c_port, config->sda_gpio, config->scl_gpio, config->rover_addr);
    return ESP_OK;
}
