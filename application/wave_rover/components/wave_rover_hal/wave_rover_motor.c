/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_hal.h"
#include "wave_rover_hal_internal.h"
#include "board_config.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <math.h>

static const char *TAG = "wr_motor";

static float s_left  = 0.0f;
static float s_right = 0.0f;
static bool  s_estop = false;
static bool  s_hw_init = false;

/* Worker task resources (Task 7b) */
#define MOTOR_CMD_QUEUE_DEPTH    4
#define MOTOR_TICK_MS           50
#define MOTOR_RESULT_QUEUE_DEPTH 4

typedef struct {
    uint32_t  req_id;
    esp_err_t err;
} wr_motor_result_t;

static QueueHandle_t     s_cmd_queue    = NULL;
static QueueHandle_t     s_result_queue = NULL;
static SemaphoreHandle_t s_queue_lock   = NULL;
static TaskHandle_t      s_worker_task  = NULL;
static uint32_t          s_req_seq      = 0;

/* ------------------------------------------------------------------ */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static esp_err_t hw_motor_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode       = WR_LEDC_SPEED,
        .duty_resolution  = WR_LEDC_BITS,
        .timer_num        = WR_LEDC_TIMER,
        .freq_hz          = WR_LEDC_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    ledc_channel_config_t chA = {
        .gpio_num   = WR_MOTOR_PWMA,
        .speed_mode = WR_LEDC_SPEED,
        .channel    = WR_LEDC_CH_A,
        .timer_sel  = WR_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config_t chB = chA;
    chB.gpio_num = WR_MOTOR_PWMB;
    chB.channel  = WR_LEDC_CH_B;

    ESP_RETURN_ON_ERROR(ledc_channel_config(&chA), TAG, "ledc ch A");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&chB), TAG, "ledc ch B");

    gpio_config_t dir = {
        .pin_bit_mask = (1ULL << WR_MOTOR_AIN1) | (1ULL << WR_MOTOR_AIN2) |
                        (1ULL << WR_MOTOR_BIN1) | (1ULL << WR_MOTOR_BIN2),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&dir), TAG, "dir gpio");

    gpio_set_level(WR_MOTOR_AIN1, 0);
    gpio_set_level(WR_MOTOR_AIN2, 0);
    gpio_set_level(WR_MOTOR_BIN1, 0);
    gpio_set_level(WR_MOTOR_BIN2, 0);
    s_hw_init = true;
    return ESP_OK;
}

static void hw_set_side(float spd, ledc_channel_t ch, int in1, int in2)
{
    if (!s_hw_init) return;
    int pwm = (int)roundf(fabsf(spd) * 255.0f);
    if (pwm > 255) pwm = 255;
    if (spd > 0.01f) {
        gpio_set_level(in1, 1);
        gpio_set_level(in2, 0);
    } else if (spd < -0.01f) {
        gpio_set_level(in1, 0);
        gpio_set_level(in2, 1);
    } else {
        gpio_set_level(in1, 0);
        gpio_set_level(in2, 0);
        pwm = 0;
    }
    ledc_set_duty(WR_LEDC_SPEED, ch, pwm);
    ledc_update_duty(WR_LEDC_SPEED, ch);
}

/* Internal raw set — bypasses s_estop guard; worker task checks s_estop in loop */
static void wr_motor_set_raw(float left, float right)
{
    bool was_stopped = (fabsf(s_left) < 0.01f && fabsf(s_right) < 0.01f);
    bool will_stop   = (fabsf(left)   < 0.01f && fabsf(right)   < 0.01f);
    int64_t t = esp_timer_get_time();

    s_left  = left;
    s_right = right;
    if (!s_hw_init) hw_motor_init();
    hw_set_side(left,  WR_LEDC_CH_A, WR_MOTOR_AIN1, WR_MOTOR_AIN2);
    hw_set_side(right, WR_LEDC_CH_B, WR_MOTOR_BIN1, WR_MOTOR_BIN2);

    if (was_stopped && !will_stop)
        ESP_LOGI(TAG, "MOTOR ON  t=%lldus L=%.2f R=%.2f", t, (double)left, (double)right);
    else if (!was_stopped && will_stop)
        ESP_LOGI(TAG, "MOTOR OFF t=%lldus", t);
    else if (!will_stop)
        ESP_LOGD(TAG, "MOTOR SET t=%lldus L=%.2f R=%.2f", t, (double)left, (double)right);
}

/* ------------------------------------------------------------------ */
/* Worker task (Task 7b)                                               */
/* ------------------------------------------------------------------ */

static void motor_worker(void *arg)
{
    wr_motor_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        esp_err_t err = ESP_OK;

        if (cmd.type == WR_MOTOR_CMD_STOP) {
            wr_motor_stop();
        } else if (cmd.type == WR_MOTOR_CMD_MOVE) {
            TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(cmd.duration_ms);
            while ((int32_t)(end - xTaskGetTickCount()) > 0) {
                if (s_estop) { err = ESP_ERR_INVALID_STATE; break; }
                wr_motor_set_raw(cmd.left, cmd.right);
                vTaskDelay(pdMS_TO_TICKS(MOTOR_TICK_MS));
            }
            wr_motor_stop();
        }

        wr_motor_result_t res = { .req_id = cmd.req_id, .err = err };
        if (xQueueSend(s_result_queue, &res, 0) != pdTRUE) {
            wr_motor_result_t dropped;
            xQueueReceive(s_result_queue, &dropped, 0);
            xQueueSend(s_result_queue, &res, 0);
        }
    }
}

esp_err_t wr_motor_worker_start(void)
{
    s_cmd_queue    = xQueueCreate(MOTOR_CMD_QUEUE_DEPTH,    sizeof(wr_motor_cmd_t));
    s_result_queue = xQueueCreate(MOTOR_RESULT_QUEUE_DEPTH, sizeof(wr_motor_result_t));
    s_queue_lock   = xSemaphoreCreateMutex();
    if (!s_cmd_queue || !s_result_queue || !s_queue_lock) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(motor_worker, "wr_motor", 4096, NULL, 5, &s_worker_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t wr_motor_submit_and_wait(const wr_motor_cmd_t *cmd_in, uint32_t timeout_ms)
{
    if (!s_cmd_queue || !s_result_queue || !s_queue_lock) return ESP_ERR_INVALID_STATE;
    if (s_estop && cmd_in->type == WR_MOTOR_CMD_MOVE) return ESP_ERR_INVALID_STATE;

    wr_motor_cmd_t cmd = *cmd_in;
    cmd.req_id = ++s_req_seq;

    xSemaphoreTake(s_queue_lock, portMAX_DELAY);
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreGive(s_queue_lock);
        return ESP_ERR_TIMEOUT;
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    esp_err_t result = ESP_ERR_TIMEOUT;
    while (1) {
        wr_motor_result_t res;
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) break;
        if (xQueueReceive(s_result_queue, &res, deadline - now) == pdTRUE) {
            if (res.req_id == cmd.req_id) { result = res.err; break; }
        }
    }
    xSemaphoreGive(s_queue_lock);
    return result;
}

/* ------------------------------------------------------------------ */
/* Public motor API                                                    */
/* ------------------------------------------------------------------ */

esp_err_t wr_motor_set(float left, float right)
{
    if (s_estop) return ESP_ERR_INVALID_STATE;
    left  = clampf(left,  -1.0f, 1.0f);
    right = clampf(right, -1.0f, 1.0f);
    wr_motor_set_raw(left, right);
    return ESP_OK;
}

esp_err_t wr_motor_stop(void)
{
    bool was_moving = (fabsf(s_left) > 0.01f || fabsf(s_right) > 0.01f);
    s_left = s_right = 0.0f;
    if (!s_hw_init) return ESP_OK;
    gpio_set_level(WR_MOTOR_AIN1, 0); gpio_set_level(WR_MOTOR_AIN2, 0);
    gpio_set_level(WR_MOTOR_BIN1, 0); gpio_set_level(WR_MOTOR_BIN2, 0);
    ledc_set_duty(WR_LEDC_SPEED, WR_LEDC_CH_A, 0);
    ledc_update_duty(WR_LEDC_SPEED, WR_LEDC_CH_A);
    ledc_set_duty(WR_LEDC_SPEED, WR_LEDC_CH_B, 0);
    ledc_update_duty(WR_LEDC_SPEED, WR_LEDC_CH_B);
    if (was_moving)
        ESP_LOGI(TAG, "MOTOR STOP t=%lldus", esp_timer_get_time());
    return ESP_OK;
}

esp_err_t wr_motor_get_state(wr_motor_state_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    s->left           = s_left;
    s->right          = s_right;
    s->emergency_stop = s_estop;
    return ESP_OK;
}

void wr_motor_emergency_stop_set(void)
{
    s_estop = true;
    wr_motor_stop();
    ESP_LOGW(TAG, "EMERGENCY STOP SET");
}

void wr_motor_emergency_stop_clear(void)
{
    s_estop = false;
    ESP_LOGI(TAG, "emergency stop cleared");
}

bool wr_motor_emergency_stop_active(void) { return s_estop; }
