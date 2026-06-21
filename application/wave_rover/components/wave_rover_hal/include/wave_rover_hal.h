/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Motor state */
typedef struct {
    float left;
    float right;
    bool  emergency_stop;
} wr_motor_state_t;

/* Power status */
typedef struct {
    bool  present;
    float bus_voltage_v;
    float shunt_voltage_mv;
    float current_ma;
    float power_mw;
    float load_voltage_v;
    bool  charging;
    bool  low_battery;
} wr_power_status_t;

/* IMU sample */
typedef struct {
    bool  present;
    float accel_x, accel_y, accel_z;   /* g */
    float gyro_x, gyro_y, gyro_z;      /* dps */
    bool  mag_present;
    float mag_x, mag_y, mag_z;         /* uT */
    float temperature_c;
    bool  has_temperature;
} wr_imu_sample_t;

/* IMU calibration result */
typedef struct {
    bool  valid;
    float gyro_offset_x, gyro_offset_y, gyro_offset_z;  /* dps bias */
    float accel_ref_z;   /* g — should be ~1.0 when flat */
} wr_imu_calib_t;

/* Motor command queue types (Task 7b: safe concurrent stop) */
typedef enum {
    WR_MOTOR_CMD_MOVE = 1,
    WR_MOTOR_CMD_STOP = 2,
} wr_motor_cmd_type_t;

typedef struct {
    wr_motor_cmd_type_t type;
    float    left;
    float    right;
    uint16_t duration_ms;
    uint32_t req_id;   /* set by wr_motor_submit_and_wait */
} wr_motor_cmd_t;

/* HAL lifecycle */
esp_err_t wr_hal_init(void);

/* Motor basic */
esp_err_t wr_motor_set(float left, float right);
esp_err_t wr_motor_stop(void);
esp_err_t wr_motor_get_state(wr_motor_state_t *state);
void      wr_motor_emergency_stop_set(void);
void      wr_motor_emergency_stop_clear(void);
bool      wr_motor_emergency_stop_active(void);

/* Motor worker task — submit timed move and wait for result */
esp_err_t wr_motor_worker_start(void);
esp_err_t wr_motor_submit_and_wait(const wr_motor_cmd_t *cmd, uint32_t timeout_ms);

/* Power */
esp_err_t wr_power_get_status(wr_power_status_t *status);

/* IMU */
esp_err_t wr_imu_get_sample(wr_imu_sample_t *sample);
/* Fast gyro-only read: bias-corrected Z-axis, for tight control loops */
esp_err_t wr_imu_get_gyro(float *gz_dps);
/* Collect `samples` readings at `interval_ms` apart; compute+store gyro bias offsets */
esp_err_t wr_imu_calibrate(uint16_t samples, uint32_t interval_ms, wr_imu_calib_t *out);
/* Switches AK09918 magnetometer between continuous 10Hz mode (true) and
 * power-down mode (false). No-op (returns ESP_OK) if magnetometer not
 * present. */
esp_err_t wr_imu_set_mag_continuous(bool enable);

/* Navigation calibration state */
typedef struct {
    float k_dist;       /* cm / (speed_unit × ms); 0 = uncalibrated */
    float gyro_scale;   /* physical_deg / integrated_deg correction (1.0 = no correction) */
    bool  k_dist_valid;
} wr_nav_cal_t;

/* Load calibration from NVS (call after nvs_flash_init) */
esp_err_t wr_nav_init(void);
/* Rotate in place: degrees>0=CCW, degrees<0=CW.
 * Calibrates gyro bias first (rover must be still).
 * integrated_out: total integrated angle (may differ from target due to lead-stop). */
esp_err_t wr_nav_rotate_deg(float degrees, float speed, float *integrated_out);
/* Drive straight forward using time-based open-loop with calibrated k_dist. */
esp_err_t wr_nav_drive_cm(float cm, float speed);
/* Persist calibration to NVS.  gyro_scale<=0 defaults to 1.0. */
esp_err_t wr_nav_set_cal(float k_dist, float gyro_scale);
wr_nav_cal_t wr_nav_get_cal(void);

/* Display */
esp_err_t wr_display_clear(void);
esp_err_t wr_display_text(const char *text, int line, bool clear_first);
esp_err_t wr_display_status(const char *fw_ver, const char *wifi_info,
                             float battery_v, bool mcp_active, bool estop);
/* SSD1306 display on/off via command 0xAF/0xAE. Framebuffer content is
 * preserved and reappears when re-enabled with `on=true`. */
esp_err_t wr_display_set_power(bool on);

#ifdef __cplusplus
}
#endif
