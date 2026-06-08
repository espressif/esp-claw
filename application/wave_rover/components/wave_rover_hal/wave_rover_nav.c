/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_hal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "wr_nav";

#define NAV_NVS_NS         "wr_nav"
#define NAV_NVS_KEY_KDIST  "k_dist"
#define NAV_NVS_KEY_GSCALE "gyro_scale"

/* Rotation loop: 20 ms period (50 Hz) */
#define NAV_LOOP_MS          20

/* Stop this many degrees before target to compensate for motor coast.
 * Tunable: measure overshoot on first run and adjust. */
#define NAV_LEAD_STOP_DEG    2.5f

/* Timeout: target/20 + 3 seconds (assumes ≥20 dps minimum gyro rate). */
#define NAV_MIN_RATE_DPS     20.0f
#define NAV_TIMEOUT_EXTRA_S  3.0f

/* Stall detection: abort if less than this many degrees accumulated per window. */
#define NAV_STALL_WINDOW_MS  2000
#define NAV_STALL_THRESHOLD  0.5f

/* Drive loop check period */
#define NAV_DRIVE_LOOP_MS    20

static float s_k_dist     = 0.0f;  /* cm / (speed_unit * ms); 0 = uncalibrated */
static float s_gyro_scale = 1.0f;  /* physical_deg / integrated_deg */

/* Load u32 bits from NVS as float via memcpy (ISO-safe IEEE 754 pun). */
static float nvs_get_float_or(nvs_handle_t h, const char *key, float def)
{
    uint32_t v;
    if (nvs_get_u32(h, key, &v) != ESP_OK) return def;
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

esp_err_t wr_nav_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAV_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        s_k_dist     = nvs_get_float_or(h, NAV_NVS_KEY_KDIST,  0.0f);
        s_gyro_scale = nvs_get_float_or(h, NAV_NVS_KEY_GSCALE, 1.0f);
        nvs_close(h);
    }
    /* Sanity-clamp scale: must be in (0.1, 10] */
    if (s_gyro_scale < 0.1f || s_gyro_scale > 10.0f) s_gyro_scale = 1.0f;

    ESP_LOGI(TAG, "nav_init: k_dist=%.6f gyro_scale=%.4f %s",
             s_k_dist, s_gyro_scale,
             (s_k_dist > 1e-6f) ? "" : "(distance not calibrated)");
    return ESP_OK;
}

esp_err_t wr_nav_set_cal(float k_dist, float gyro_scale)
{
    if (gyro_scale <= 0.0f) gyro_scale = 1.0f;
    s_k_dist     = k_dist;
    s_gyro_scale = gyro_scale;

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NAV_NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    uint32_t v;
    memcpy(&v, &k_dist,      sizeof(v)); nvs_set_u32(h, NAV_NVS_KEY_KDIST,  v);
    memcpy(&v, &gyro_scale,  sizeof(v)); nvs_set_u32(h, NAV_NVS_KEY_GSCALE, v);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "nav cal saved: k_dist=%.6f gyro_scale=%.4f", s_k_dist, s_gyro_scale);
    return err;
}

wr_nav_cal_t wr_nav_get_cal(void)
{
    return (wr_nav_cal_t){
        .k_dist       = s_k_dist,
        .gyro_scale   = s_gyro_scale,
        .k_dist_valid = (s_k_dist > 1e-6f),
    };
}

esp_err_t wr_nav_rotate_deg(float degrees, float speed, float *integrated_out)
{
    if (integrated_out) *integrated_out = 0.0f;
    if (wr_motor_emergency_stop_active()) return ESP_ERR_NOT_ALLOWED;

    float target = fabsf(degrees);
    if (target < 0.5f) return ESP_OK;

    /* Calibrate gyro bias: 20 samples × 10 ms = 200 ms (rover must be still) */
    wr_imu_calib_t bias = {0};
    wr_imu_calibrate(20, 10, &bias);

    float sign = (degrees > 0.0f) ? 1.0f : -1.0f;

    /* CCW (degrees>0): left=-speed, right=+speed  (matches our gyro test results) */
    wr_motor_set(-sign * speed, sign * speed);

    float lead = NAV_LEAD_STOP_DEG;
    if (lead >= target * 0.9f) lead = target * 0.1f;
    float stop_at = target - lead;

    float   accumulated       = 0.0f;
    float   stall_base        = 0.0f;
    int64_t start_us          = esp_timer_get_time();
    int64_t last_us           = start_us;
    int64_t stall_window_us   = start_us;
    float   timeout_s         = target / NAV_MIN_RATE_DPS + NAV_TIMEOUT_EXTRA_S;

    while (accumulated < stop_at) {
        vTaskDelay(pdMS_TO_TICKS(NAV_LOOP_MS));

        if (wr_motor_emergency_stop_active()) {
            wr_motor_stop();
            if (integrated_out) *integrated_out = accumulated;
            return ESP_ERR_NOT_ALLOWED;
        }

        int64_t now_us = esp_timer_get_time();

        /* Global timeout */
        if ((float)(now_us - start_us) * 1e-6f > timeout_s) {
            ESP_LOGW(TAG, "rotate_deg: timeout at %.1f°/%.1f°", accumulated, target);
            break;
        }

        float gz = 0.0f;
        if (wr_imu_get_gyro(&gz) == ESP_OK) {
            float dt_s = (float)(now_us - last_us) * 1e-6f;
            float contribution = sign * gz * s_gyro_scale * dt_s;
            if (contribution > 0.0f) accumulated += contribution;
        }
        last_us = now_us;

        /* Stall detection: if progress < threshold over stall window, abort */
        if (now_us - stall_window_us > (int64_t)NAV_STALL_WINDOW_MS * 1000LL) {
            if (accumulated - stall_base < NAV_STALL_THRESHOLD) {
                ESP_LOGW(TAG, "rotate_deg: stall at %.1f°/%.1f°, aborting",
                         accumulated, target);
                break;
            }
            stall_base      = accumulated;
            stall_window_us = now_us;
        }

        /* Re-assert motor command each iteration to survive any external stop */
        wr_motor_set(-sign * speed, sign * speed);
    }

    wr_motor_stop();

    if (integrated_out) *integrated_out = accumulated;
    ESP_LOGI(TAG, "rotate_deg: target=%.1f° lead_stop=%.1f° accumulated=%.1f° scale=%.4f",
             target, lead, accumulated, s_gyro_scale);
    return ESP_OK;
}

esp_err_t wr_nav_drive_cm(float cm, float speed)
{
    if (wr_motor_emergency_stop_active()) return ESP_ERR_NOT_ALLOWED;
    if (s_k_dist < 1e-6f)               return ESP_ERR_INVALID_STATE;
    if (cm <= 0.0f)                      return ESP_ERR_INVALID_ARG;
    if (speed <= 0.0f || speed > 1.0f)  speed = 0.4f;

    int32_t dur_ms = (int32_t)(cm / (s_k_dist * speed));
    if (dur_ms < 10)    dur_ms = 10;
    if (dur_ms > 30000) dur_ms = 30000;  /* 30 s safety cap */

    ESP_LOGI(TAG, "drive_cm: %.1f cm speed=%.2f → %ld ms", cm, speed, (long)dur_ms);

    wr_motor_set(speed, speed);

    int64_t end_us = esp_timer_get_time() + (int64_t)dur_ms * 1000LL;

    while (esp_timer_get_time() < end_us) {
        vTaskDelay(pdMS_TO_TICKS(NAV_DRIVE_LOOP_MS));
        if (wr_motor_emergency_stop_active()) {
            wr_motor_stop();
            return ESP_ERR_NOT_ALLOWED;
        }
    }

    wr_motor_stop();
    return ESP_OK;
}
