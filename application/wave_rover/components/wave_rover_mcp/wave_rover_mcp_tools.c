/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_hal.h"
#include "wave_rover_config.h"
#include "wave_rover_mcp_state.h"
#include "wave_rover_power_mgr.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wr_tools";

static const wave_rover_config_t *s_cfg       = NULL;
static wave_rover_config_t       *s_cfg_mut   = NULL;
static wr_power_mgr_handle_t      s_power_mgr = NULL;

void wr_mcp_tools_set_config(const wave_rover_config_t *cfg) { s_cfg = cfg; }
void wr_mcp_tools_set_config_mut(wave_rover_config_t *cfg)   { s_cfg_mut = cfg; }
void wr_mcp_tools_set_power_mgr(wr_power_mgr_handle_t pm)   { s_power_mgr = pm; }

#define WR_FW_VERSION "0.1.0"
#define WR_FW_NAME    "wave-rover-esp-claw-mcp"

/* ------------------------------------------------------------------ */
/* JSON helpers                                                        */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t json_obj_str(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    esp_mcp_value_t v = esp_mcp_value_create_string(s ? s : "{}");
    free(s);
    cJSON_Delete(obj);
    return v;
}

/* ------------------------------------------------------------------ */
/* rover.get_status                                                    */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_get_status(const esp_mcp_property_list_t *p)
{
    (void)p;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "state",
        wr_motor_emergency_stop_active() ? "emergency_stop" : "idle");

    cJSON *fw = cJSON_AddObjectToObject(root, "firmware");
    cJSON_AddStringToObject(fw, "name",    WR_FW_NAME);
    cJSON_AddStringToObject(fw, "version", WR_FW_VERSION);

    wr_motor_state_t ms = {0};
    wr_motor_get_state(&ms);
    cJSON *mot = cJSON_AddObjectToObject(root, "motors");
    cJSON_AddNumberToObject(mot, "left",            ms.left);
    cJSON_AddNumberToObject(mot, "right",           ms.right);
    cJSON_AddBoolToObject(mot,   "emergency_stop",  ms.emergency_stop);

    wr_power_status_t ps = {0};
    wr_power_get_status(&ps);
    cJSON *pwr = cJSON_AddObjectToObject(root, "power");
    cJSON_AddBoolToObject(pwr,   "present",     ps.present);
    cJSON_AddNumberToObject(pwr, "voltage",     ps.load_voltage_v);
    cJSON_AddNumberToObject(pwr, "current_ma",  ps.current_ma);
    cJSON_AddBoolToObject(pwr,   "charging",    ps.charging);
    cJSON_AddBoolToObject(pwr,   "low_battery", ps.low_battery);

    wr_imu_sample_t is = {0};
    wr_imu_get_sample(&is);
    cJSON *imu = cJSON_AddObjectToObject(root, "imu");
    cJSON_AddBoolToObject(imu, "present", is.present);

    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(root, "free_heap",  (double)esp_get_free_heap_size());
    cJSON_AddNullToObject(root, "last_error");

    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.get_config                                                    */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_get_config(const esp_mcp_property_list_t *p)
{
    (void)p;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    if (s_cfg) {
        cJSON_AddStringToObject(root, "hostname",     s_cfg->hostname);
        cJSON_AddNumberToObject(root, "mcp_port",     s_cfg->mcp_port);
        cJSON_AddBoolToObject(root,   "auth_enabled", s_cfg->auth_enabled);
        cJSON_AddNumberToObject(root, "max_speed",    (double)s_cfg->max_speed);
        cJSON_AddNumberToObject(root, "max_cmd_ms",   s_cfg->max_command_duration_ms);
    }
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.stop                                                          */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_stop(const esp_mcp_property_list_t *p)
{
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
    const char *reason = esp_mcp_property_list_get_property_string(p, "reason");
    wr_motor_stop();
    ESP_LOGI(TAG, "rover.stop: %s", reason ? reason : "(no reason)");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok",     true);
    cJSON_AddStringToObject(root, "action", "stopped");
    if (reason) cJSON_AddStringToObject(root, "reason", reason);
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.emergency_stop                                                */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_emergency_stop(const esp_mcp_property_list_t *p)
{
    const char *reason = esp_mcp_property_list_get_property_string(p, "reason");
    wr_motor_emergency_stop_set();
    ESP_LOGW(TAG, "rover.emergency_stop: %s", reason ? reason : "(no reason)");
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "ok",     true);
    cJSON_AddStringToObject(root, "action", "emergency_stop_set");
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.clear_emergency_stop                                          */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_clear_estop(const esp_mcp_property_list_t *p)
{
    bool confirm = esp_mcp_property_list_get_property_bool(p, "confirm");
    cJSON *root = cJSON_CreateObject();
    if (!confirm) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "confirm must be true");
        return json_obj_str(root);
    }
    wr_motor_emergency_stop_clear();
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
    cJSON_AddBoolToObject(root,   "ok",     true);
    cJSON_AddStringToObject(root, "action", "emergency_stop_cleared");
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.move                                                          */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_move(const esp_mcp_property_list_t *p)
{
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
    float linear  = (float)esp_mcp_property_list_get_property_float(p, "linear");
    float angular = (float)esp_mcp_property_list_get_property_float(p, "angular");
    int   dur_ms  = esp_mcp_property_list_get_property_int(p, "duration_ms");

    cJSON *root = cJSON_CreateObject();
    if (wr_motor_emergency_stop_active()) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "emergency_stop_active");
        return json_obj_str(root);
    }

    if (linear  < -1.0f) linear  = -1.0f;
    if (linear  >  1.0f) linear  =  1.0f;
    if (angular < -1.0f) angular = -1.0f;
    if (angular >  1.0f) angular =  1.0f;
    if (dur_ms < 1)    dur_ms = 1;
    if (dur_ms > 3000) dur_ms = 3000;

    float max_spd = s_cfg ? s_cfg->max_speed : 0.4f;
    if (linear >  max_spd) linear =  max_spd;
    if (linear < -max_spd) linear = -max_spd;

    float left  = linear - angular;
    float right = linear + angular;
    if (left  >  1.0f) left  =  1.0f;
    if (left  < -1.0f) left  = -1.0f;
    if (right >  1.0f) right =  1.0f;
    if (right < -1.0f) right = -1.0f;

    wr_motor_cmd_t cmd = {
        .type        = WR_MOTOR_CMD_MOVE,
        .left        = left,
        .right       = right,
        .duration_ms = (uint16_t)dur_ms,
    };
    esp_err_t err = wr_motor_submit_and_wait(&cmd, (uint32_t)(dur_ms + 500));

    cJSON_AddBoolToObject(root,   "ok",          err == ESP_OK);
    cJSON_AddStringToObject(root, "action",      err == ESP_OK ? "move_complete" : "preempted");
    cJSON_AddNumberToObject(root, "linear",      (double)linear);
    cJSON_AddNumberToObject(root, "angular",     (double)angular);
    cJSON_AddNumberToObject(root, "duration_ms", dur_ms);
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.drive_tank                                                    */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_drive_tank(const esp_mcp_property_list_t *p)
{
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
    float left   = (float)esp_mcp_property_list_get_property_float(p, "left");
    float right  = (float)esp_mcp_property_list_get_property_float(p, "right");
    int   dur_ms = esp_mcp_property_list_get_property_int(p, "duration_ms");

    cJSON *root = cJSON_CreateObject();
    if (wr_motor_emergency_stop_active()) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "emergency_stop_active");
        return json_obj_str(root);
    }

    float max_spd = s_cfg ? s_cfg->max_speed : 0.4f;
    if (left  >  max_spd) left  =  max_spd;
    if (left  < -max_spd) left  = -max_spd;
    if (right >  max_spd) right =  max_spd;
    if (right < -max_spd) right = -max_spd;
    if (dur_ms < 1)    dur_ms = 1;
    if (dur_ms > 3000) dur_ms = 3000;

    wr_motor_cmd_t cmd = {
        .type        = WR_MOTOR_CMD_MOVE,
        .left        = left,
        .right       = right,
        .duration_ms = (uint16_t)dur_ms,
    };
    esp_err_t err = wr_motor_submit_and_wait(&cmd, (uint32_t)(dur_ms + 500));

    cJSON_AddBoolToObject(root,   "ok",     err == ESP_OK);
    cJSON_AddStringToObject(root, "action", err == ESP_OK ? "drive_tank_complete" : "preempted");
    cJSON_AddNumberToObject(root, "left",   (double)left);
    cJSON_AddNumberToObject(root, "right",  (double)right);
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.turn                                                          */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_turn(const esp_mcp_property_list_t *p)
{
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
    const char *dir = esp_mcp_property_list_get_property_string(p, "direction");
    float spd       = (float)esp_mcp_property_list_get_property_float(p, "speed");
    int   dur_ms    = esp_mcp_property_list_get_property_int(p, "duration_ms");

    cJSON *root = cJSON_CreateObject();
    if (wr_motor_emergency_stop_active()) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "emergency_stop_active");
        return json_obj_str(root);
    }

    float max_spd = s_cfg ? s_cfg->max_speed : 0.4f;
    if (spd < 0.0f)    spd = 0.0f;
    if (spd > max_spd) spd = max_spd;
    if (dur_ms < 1)    dur_ms = 1;
    if (dur_ms > 3000) dur_ms = 3000;

    float left, right;
    if (dir && strcmp(dir, "right") == 0) {
        left  =  spd;
        right = -spd;
    } else {
        left  = -spd;
        right =  spd;
    }

    wr_motor_cmd_t cmd = {
        .type        = WR_MOTOR_CMD_MOVE,
        .left        = left,
        .right       = right,
        .duration_ms = (uint16_t)dur_ms,
    };
    esp_err_t err = wr_motor_submit_and_wait(&cmd, (uint32_t)(dur_ms + 500));

    cJSON_AddBoolToObject(root,   "ok",        err == ESP_OK);
    cJSON_AddStringToObject(root, "action",    err == ESP_OK ? "turn_complete" : "preempted");
    cJSON_AddStringToObject(root, "direction", dir ? dir : "left");
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.get_power / rover.get_ups                                     */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_get_power(const esp_mcp_property_list_t *p)
{
    (void)p;
    wr_power_status_t ps = {0};
    wr_power_get_status(&ps);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "ok",              true);
    cJSON_AddBoolToObject(root,   "present",          ps.present);
    cJSON_AddStringToObject(root, "source",           "ina219");
    cJSON_AddNumberToObject(root, "bus_voltage_v",    (double)ps.bus_voltage_v);
    cJSON_AddNumberToObject(root, "shunt_voltage_mv", (double)ps.shunt_voltage_mv);
    cJSON_AddNumberToObject(root, "current_ma",       (double)ps.current_ma);
    cJSON_AddNumberToObject(root, "power_mw",         (double)ps.power_mw);
    cJSON_AddBoolToObject(root,   "charging",         ps.charging);
    cJSON_AddBoolToObject(root,   "low_battery",      ps.low_battery);
    return json_obj_str(root);
}

static esp_mcp_value_t tool_get_ups(const esp_mcp_property_list_t *p)
{
    (void)p;
    wr_power_status_t ps = {0};
    wr_power_get_status(&ps);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "ok",          true);
    cJSON_AddBoolToObject(root,   "present",      ps.present);
    cJSON_AddStringToObject(root, "type",         "3s_18650");
    cJSON_AddStringToObject(root, "monitor",      "ina219");
    cJSON_AddNumberToObject(root, "voltage_v",    (double)ps.load_voltage_v);
    cJSON_AddNumberToObject(root, "current_ma",   (double)ps.current_ma);
    cJSON_AddNullToObject(root,   "estimated_percent");
    cJSON_AddBoolToObject(root,   "charging",     ps.charging);
    cJSON_AddBoolToObject(root,   "discharging",  !ps.charging);
    cJSON_AddBoolToObject(root,   "low_battery",  ps.low_battery);
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.get_imu                                                       */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_get_imu(const esp_mcp_property_list_t *p)
{
    (void)p;
    wr_imu_sample_t is = {0};
    wr_imu_get_sample(&is);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "ok",      true);
    cJSON_AddBoolToObject(root,   "present", is.present);
    cJSON_AddStringToObject(root, "chip",    "QMI8658+AK09918");

    cJSON *accel = cJSON_AddObjectToObject(root, "accel");
    cJSON_AddNumberToObject(accel, "x", (double)is.accel_x);
    cJSON_AddNumberToObject(accel, "y", (double)is.accel_y);
    cJSON_AddNumberToObject(accel, "z", (double)is.accel_z);

    cJSON *gyro = cJSON_AddObjectToObject(root, "gyro");
    cJSON_AddNumberToObject(gyro, "x", (double)is.gyro_x);
    cJSON_AddNumberToObject(gyro, "y", (double)is.gyro_y);
    cJSON_AddNumberToObject(gyro, "z", (double)is.gyro_z);

    cJSON *mag = cJSON_AddObjectToObject(root, "mag");
    cJSON_AddBoolToObject(mag,   "present", is.mag_present);
    cJSON_AddNumberToObject(mag, "x",       (double)is.mag_x);
    cJSON_AddNumberToObject(mag, "y",       (double)is.mag_y);
    cJSON_AddNumberToObject(mag, "z",       (double)is.mag_z);

    if (is.has_temperature)
        cJSON_AddNumberToObject(root, "temperature_c", (double)is.temperature_c);
    else
        cJSON_AddNullToObject(root, "temperature_c");

    cJSON_AddNullToObject(root, "calibration");
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.calibrate_imu                                                 */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_calibrate_imu(const esp_mcp_property_list_t *p)
{
    int samples     = esp_mcp_property_list_get_property_int(p, "samples");
    int interval_ms = esp_mcp_property_list_get_property_int(p, "interval_ms");

    if (samples     <= 0)  samples     = 50;
    if (samples     > 500) samples     = 500;
    if (interval_ms <= 0)  interval_ms = 20;

    ESP_LOGI(TAG, "rover.calibrate_imu: %d samples @ %d ms", samples, interval_ms);

    wr_imu_calib_t cal = {0};
    esp_err_t err = wr_imu_calibrate((uint16_t)samples, (uint32_t)interval_ms, &cal);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK && cal.valid);
    if (err == ESP_OK && cal.valid) {
        cJSON *offsets = cJSON_AddObjectToObject(root, "gyro_offsets_dps");
        cJSON_AddNumberToObject(offsets, "x", (double)cal.gyro_offset_x);
        cJSON_AddNumberToObject(offsets, "y", (double)cal.gyro_offset_y);
        cJSON_AddNumberToObject(offsets, "z", (double)cal.gyro_offset_z);
        cJSON_AddNumberToObject(root, "accel_ref_z_g", (double)cal.accel_ref_z);
        cJSON_AddNumberToObject(root, "samples_used",  samples);
    } else {
        cJSON_AddStringToObject(root, "error",
            err == ESP_ERR_INVALID_STATE ? "imu_not_ready" : "calibration_failed");
    }
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* Display tools                                                       */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_display_text(const esp_mcp_property_list_t *p)
{
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
    const char *text = esp_mcp_property_list_get_property_string(p, "text");
    int line         = esp_mcp_property_list_get_property_int(p, "line");
    bool clr         = esp_mcp_property_list_get_property_bool(p, "clear");
    wr_display_text(text, line, clr);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return json_obj_str(root);
}

static esp_mcp_value_t tool_display_clear(const esp_mcp_property_list_t *p)
{
    (void)p;
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
    wr_display_clear();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return json_obj_str(root);
}

static esp_mcp_value_t tool_display_status(const esp_mcp_property_list_t *p)
{
    (void)p;
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
    wr_power_status_t ps = {0};
    wr_power_get_status(&ps);
    wr_display_status(WR_FW_VERSION, "MCP active", ps.load_voltage_v,
                      true, wr_motor_emergency_stop_active());
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* Wi-Fi tools                                                         */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_get_wifi(const esp_mcp_property_list_t *p)
{
    (void)p;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    if (s_cfg) {
        cJSON_AddNumberToObject(root, "mode",     s_cfg->wifi_mode);
        cJSON_AddStringToObject(root, "ssid",     s_cfg->wifi_ssid);
        cJSON_AddStringToObject(root, "hostname", s_cfg->hostname);
        /* Never return passwords */
    }
    return json_obj_str(root);
}

static esp_mcp_value_t tool_set_wifi(const esp_mcp_property_list_t *p)
{
    const char *ssid     = esp_mcp_property_list_get_property_string(p, "ssid");
    const char *password = esp_mcp_property_list_get_property_string(p, "password");
    const char *mode_str = esp_mcp_property_list_get_property_string(p, "mode");
    bool save            = esp_mcp_property_list_get_property_bool(p, "save");

    cJSON *root = cJSON_CreateObject();
    if (!mode_str) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "mode required");
        return json_obj_str(root);
    }
    /* Validate mode strictly — unknown value must not silently change to AP */
    uint8_t wifi_mode;
    if      (strcmp(mode_str, "ap")     == 0) wifi_mode = 0;
    else if (strcmp(mode_str, "sta")    == 0) wifi_mode = 1;
    else if (strcmp(mode_str, "ap_sta") == 0) wifi_mode = 2;
    else {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "mode must be 'ap', 'sta', or 'ap_sta'");
        return json_obj_str(root);
    }

    if (s_cfg && save) {
        wave_rover_config_t new_cfg;
        memcpy(&new_cfg, s_cfg, sizeof(new_cfg));
        if (ssid)     strlcpy(new_cfg.wifi_ssid, ssid, sizeof(new_cfg.wifi_ssid));
        /* do not log password */
        if (password) strlcpy(new_cfg.wifi_password, password, sizeof(new_cfg.wifi_password));
        new_cfg.wifi_mode = wifi_mode;

        /* STA/AP_STA with an empty SSID makes esp_wifi_connect() fail with
         * ESP_ERR_WIFI_SSID at boot — wr_wifi_init cannot recover from that,
         * and a saved bad config crash-loops the device on every reboot
         * (this exact bug bricked the rover on 2026-06-08). Reject the save
         * up front instead of persisting an unbootable combination. */
        if ((wifi_mode == 1 || wifi_mode == 2) && new_cfg.wifi_ssid[0] == '\0') {
            cJSON_AddBoolToObject(root,   "ok",    false);
            cJSON_AddStringToObject(root, "error", "ssid required for sta/ap_sta mode");
            return json_obj_str(root);
        }

        wave_rover_config_save(&new_cfg);
        ESP_LOGI(TAG, "wifi config saved, reboot to apply");
    }

    /* do NOT return password in response */
    cJSON_AddBoolToObject(root,   "ok",   true);
    cJSON_AddStringToObject(root, "note", "reboot_to_apply");
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.rotate_deg                                                    */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_rotate_deg(const esp_mcp_property_list_t *p)
{
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
    float angle_deg = (float)esp_mcp_property_list_get_property_float(p, "angle_deg");
    float speed     = (float)esp_mcp_property_list_get_property_float(p, "speed");

    float max_spd = s_cfg ? s_cfg->max_speed : 0.4f;
    if (speed <= 0.0f || speed > max_spd) speed = max_spd;

    cJSON *root = cJSON_CreateObject();
    if (wr_motor_emergency_stop_active()) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "emergency_stop_active");
        return json_obj_str(root);
    }

    float integrated = 0.0f;
    wr_rover_state_set_nav_busy(true);
    esp_err_t err = wr_nav_rotate_deg(angle_deg, speed, &integrated);
    wr_rover_state_set_nav_busy(false);

    cJSON_AddBoolToObject(root,   "ok",             err == ESP_OK);
    cJSON_AddStringToObject(root, "action",         err == ESP_OK ? "rotated" : "error");
    cJSON_AddNumberToObject(root, "target_deg",     (double)angle_deg);
    cJSON_AddNumberToObject(root, "integrated_deg", (double)integrated);
    cJSON_AddStringToObject(root, "direction",      angle_deg > 0 ? "CCW" : "CW");
    if (err != ESP_OK)
        cJSON_AddStringToObject(root, "error",
            err == ESP_ERR_NOT_ALLOWED ? "emergency_stop" : "timeout");
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.drive_cm                                                      */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_drive_cm(const esp_mcp_property_list_t *p)
{
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
    float distance_cm = (float)esp_mcp_property_list_get_property_float(p, "distance_cm");
    float speed       = (float)esp_mcp_property_list_get_property_float(p, "speed");

    float max_spd = s_cfg ? s_cfg->max_speed : 0.4f;
    if (speed <= 0.0f || speed > max_spd) speed = max_spd;

    cJSON *root = cJSON_CreateObject();
    if (wr_motor_emergency_stop_active()) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "emergency_stop_active");
        return json_obj_str(root);
    }
    wr_nav_cal_t cal = wr_nav_get_cal();
    if (!cal.k_dist_valid) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "distance_not_calibrated");
        cJSON_AddStringToObject(root, "hint",  "call rover.set_nav_cal with k_dist");
        return json_obj_str(root);
    }

    int32_t dur_ms = (int32_t)(distance_cm / (cal.k_dist * speed));
    wr_rover_state_set_nav_busy(true);
    esp_err_t err = wr_nav_drive_cm(distance_cm, speed);
    wr_rover_state_set_nav_busy(false);

    cJSON_AddBoolToObject(root,   "ok",          err == ESP_OK);
    cJSON_AddStringToObject(root, "action",      err == ESP_OK ? "drive_complete" : "error");
    cJSON_AddNumberToObject(root, "distance_cm", (double)distance_cm);
    cJSON_AddNumberToObject(root, "speed",       (double)speed);
    cJSON_AddNumberToObject(root, "duration_ms", (double)dur_ms);
    if (err != ESP_OK)
        cJSON_AddStringToObject(root, "error",
            err == ESP_ERR_NOT_ALLOWED ? "emergency_stop" : "drive_failed");
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.nav_to  (drive straight then rotate)                         */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_nav_to(const esp_mcp_property_list_t *p)
{
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
    float distance_cm = (float)esp_mcp_property_list_get_property_float(p, "distance_cm");
    float angle_deg   = (float)esp_mcp_property_list_get_property_float(p, "angle_deg");
    float speed       = (float)esp_mcp_property_list_get_property_float(p, "speed");

    float max_spd = s_cfg ? s_cfg->max_speed : 0.4f;
    if (speed <= 0.0f || speed > max_spd) speed = max_spd;

    cJSON *root = cJSON_CreateObject();
    if (wr_motor_emergency_stop_active()) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "emergency_stop_active");
        return json_obj_str(root);
    }

    bool need_drive = (distance_cm > 0.01f);
    wr_nav_cal_t cal = {0};
    if (need_drive) {
        cal = wr_nav_get_cal();
        if (!cal.k_dist_valid) {
            cJSON_AddBoolToObject(root,   "ok",    false);
            cJSON_AddStringToObject(root, "error", "distance_not_calibrated");
            cJSON_AddStringToObject(root, "hint",  "call rover.set_nav_cal with k_dist");
            return json_obj_str(root);
        }
    }

    bool drive_ok = true;
    bool rotate_ok = true;
    float integrated_deg = 0.0f;

    wr_rover_state_set_nav_busy(true);
    if (need_drive) {
        drive_ok = (wr_nav_drive_cm(distance_cm, speed) == ESP_OK);
    }
    if (drive_ok && fabsf(angle_deg) > 0.5f) {
        rotate_ok = (wr_nav_rotate_deg(angle_deg, speed, &integrated_deg) == ESP_OK);
    }
    wr_rover_state_set_nav_busy(false);

    cJSON_AddBoolToObject(root,   "ok",             drive_ok && rotate_ok);
    cJSON_AddBoolToObject(root,   "drive_ok",       drive_ok);
    cJSON_AddBoolToObject(root,   "rotate_ok",      rotate_ok);
    cJSON_AddNumberToObject(root, "distance_cm",    (double)distance_cm);
    cJSON_AddNumberToObject(root, "angle_deg",      (double)angle_deg);
    cJSON_AddNumberToObject(root, "integrated_deg", (double)integrated_deg);
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.get_nav_cal / rover.set_nav_cal                               */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_get_nav_cal(const esp_mcp_property_list_t *p)
{
    (void)p;
    wr_nav_cal_t cal = wr_nav_get_cal();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "ok",           true);
    cJSON_AddNumberToObject(root, "k_dist",       (double)cal.k_dist);
    cJSON_AddNumberToObject(root, "gyro_scale",   (double)cal.gyro_scale);
    cJSON_AddBoolToObject(root,   "k_dist_valid", cal.k_dist_valid);
    cJSON_AddStringToObject(root, "hint_k_dist",  "cm / (speed_unit * ms)");
    cJSON_AddStringToObject(root, "hint_gyro",    "physical_deg / integrated_deg");
    return json_obj_str(root);
}

static esp_mcp_value_t tool_set_nav_cal(const esp_mcp_property_list_t *p)
{
    float k_dist     = (float)esp_mcp_property_list_get_property_float(p, "k_dist");
    float gyro_scale = (float)esp_mcp_property_list_get_property_float(p, "gyro_scale");
    if (gyro_scale <= 0.0f) gyro_scale = 1.0f;

    esp_err_t err = wr_nav_set_cal(k_dist, gyro_scale);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "ok",         err == ESP_OK);
    cJSON_AddNumberToObject(root, "k_dist",     (double)k_dist);
    cJSON_AddNumberToObject(root, "gyro_scale", (double)gyro_scale);
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.power_get_status                                              */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_power_get_status(const esp_mcp_property_list_t *p)
{
    (void)p;
    char buf[256];
    if (!s_power_mgr ||
        wr_power_mgr_get_status_json(s_power_mgr, buf, sizeof(buf)) != ESP_OK) {
        return esp_mcp_value_create_string("{\"error\":\"power manager unavailable\"}");
    }
    return esp_mcp_value_create_string(buf);
}

/* ------------------------------------------------------------------ */
/* rover.power_set_mode                                                */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_power_set_mode(const esp_mcp_property_list_t *p)
{
    const char *mode_str = esp_mcp_property_list_get_property_string(p, "mode");
    const char *reason   = esp_mcp_property_list_get_property_string(p, "reason");

    wr_power_mode_t mode;
    if      (mode_str && strcasecmp(mode_str, "ACTIVE")    == 0) mode = WR_POWER_MODE_ACTIVE;
    else if (mode_str && strcasecmp(mode_str, "IDLE")      == 0) mode = WR_POWER_MODE_IDLE;
    else if (mode_str && strcasecmp(mode_str, "LOW_POWER") == 0) mode = WR_POWER_MODE_LOW_POWER;
    else return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"invalid mode\"}");

    if (!s_power_mgr)
        return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"power manager unavailable\"}");

    esp_err_t err = wr_power_mgr_set_mode(s_power_mgr, mode, reason ? reason : "mcp");
    if (err != ESP_OK)
        return esp_mcp_value_create_string(
            "{\"ok\":false,\"error\":\"rejected: rover busy or sleep lock held\"}");

    char out[80];
    snprintf(out, sizeof(out), "{\"ok\":true,\"mode\":\"%s\"}", wr_power_mode_name(mode));
    return esp_mcp_value_create_string(out);
}

/* ------------------------------------------------------------------ */
/* rover.power_configure                                               */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_power_configure(const esp_mcp_property_list_t *p)
{
    if (!s_cfg_mut)
        return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"config unavailable\"}");

    wave_rover_config_t cfg = *s_cfg_mut;

    /* For integer/float fields: 0 / 0.0 means "not provided" — skip those.
     * For boolean fields: always applied when the property is present (callers
     * must include all bools they want to set). */
    int v_int;
    float v_float;

    v_int = esp_mcp_property_list_get_property_int(p, "active_timeout_sec");
    if (v_int > 0) cfg.power_active_timeout_sec = (uint16_t)v_int;

    v_int = esp_mcp_property_list_get_property_int(p, "idle_to_low_power_sec");
    if (v_int > 0) cfg.power_idle_to_low_power_sec = (uint16_t)v_int;

    v_float = esp_mcp_property_list_get_property_float(p, "critical_battery_voltage");
    if (v_float > 0.0f) cfg.power_critical_battery_v = v_float;

    /* Booleans: applied unconditionally when the property is registered.
     * Callers should include all three when changing any of them. */
    cfg.power_wifi_power_save    = esp_mcp_property_list_get_property_bool(p, "wifi_power_save");
    cfg.power_reduce_cpu_frequency = esp_mcp_property_list_get_property_bool(p, "reduce_cpu_frequency");
    cfg.power_disable_display_idle  = esp_mcp_property_list_get_property_bool(p, "disable_display_when_idle");

    esp_err_t err = wave_rover_config_save(&cfg);
    if (err != ESP_OK)
        return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"save failed\"}");

    *s_cfg_mut = cfg;
    return esp_mcp_value_create_string(
        "{\"ok\":true,\"note\":\"some settings take effect after reboot\"}");
}

/* ------------------------------------------------------------------ */
/* rover.power_prevent_sleep                                           */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_power_prevent_sleep(const esp_mcp_property_list_t *p)
{
    const char *reason = esp_mcp_property_list_get_property_string(p, "reason");
    int ttl_sec        = esp_mcp_property_list_get_property_int(p, "ttl_sec");
    if (!reason) reason = "manual_control";
    if (ttl_sec < 0) ttl_sec = 0;

    if (!s_power_mgr)
        return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"power manager unavailable\"}");

    uint32_t lock_id = 0;
    esp_err_t err = wr_power_mgr_acquire_lock(s_power_mgr, reason, (uint32_t)ttl_sec, &lock_id);
    if (err != ESP_OK)
        return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"too many locks\"}");

    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"lock_id\":%lu}", (unsigned long)lock_id);
    return esp_mcp_value_create_string(out);
}

/* ------------------------------------------------------------------ */
/* rover.power_release_sleep_lock                                      */
/* ------------------------------------------------------------------ */

static esp_mcp_value_t tool_power_release_sleep_lock(const esp_mcp_property_list_t *p)
{
    int lock_id = esp_mcp_property_list_get_property_int(p, "lock_id");
    if (!s_power_mgr)
        return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"power manager unavailable\"}");

    esp_err_t err = wr_power_mgr_release_lock(s_power_mgr, (uint32_t)lock_id);
    if (err != ESP_OK)
        return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"lock not found\"}");

    return esp_mcp_value_create_string("{\"ok\":true}");
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

#define TOOL(name, desc, cb) do { \
    esp_mcp_tool_t *_t = esp_mcp_tool_create((name), (desc), (cb)); \
    if (!_t) return ESP_ERR_NO_MEM; \
    if (esp_mcp_add_tool(mcp, _t) != ESP_OK) { esp_mcp_tool_destroy(_t); return ESP_FAIL; } \
} while (0)

#define PROP_STR(t, name)            esp_mcp_tool_add_property((t), esp_mcp_property_create_with_string((name), ""))
#define PROP_FLOAT(t, name)          esp_mcp_tool_add_property((t), esp_mcp_property_create_with_float((name), 0.0f))
#define PROP_BOOL(t, name)           esp_mcp_tool_add_property((t), esp_mcp_property_create_with_bool((name), false))
#define PROP_INT(t, name)            esp_mcp_tool_add_property((t), esp_mcp_property_create_with_int((name), 0))
#define PROP_INT_RANGE(t, n, lo, hi) esp_mcp_tool_add_property((t), esp_mcp_property_create_with_range((n), (lo), (hi)))

esp_err_t wr_mcp_register_all_tools(esp_mcp_t *mcp)
{
    /* System */
    TOOL("rover.get_status", "Get full rover status",   tool_get_status);
    TOOL("rover.get_config", "Get rover configuration", tool_get_config);

    /* Motor — float params use create_with_float (range is int-only) */
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.move",
                                                 "Move rover (linear+angular)", tool_move);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_FLOAT(t, "linear");
        PROP_FLOAT(t, "angular");
        PROP_INT_RANGE(t, "duration_ms", 1, 3000);
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.drive_tank",
                                                 "Tank drive (left+right)", tool_drive_tank);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_FLOAT(t, "left");
        PROP_FLOAT(t, "right");
        PROP_INT_RANGE(t, "duration_ms", 1, 3000);
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.turn",
                                                 "Turn in place", tool_turn);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_STR(t, "direction");
        PROP_FLOAT(t, "speed");
        PROP_INT_RANGE(t, "duration_ms", 1, 3000);
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }
    TOOL("rover.stop",           "Stop all motors",                  tool_stop);
    TOOL("rover.emergency_stop", "Emergency stop (blocks movement)", tool_emergency_stop);
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.clear_emergency_stop",
                                                 "Clear emergency stop flag",
                                                 tool_clear_estop);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_BOOL(t, "confirm");
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }

    /* Precise navigation */
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.rotate_deg",
            "Rotate in place by exact degrees using gyro integration. "
            "angle_deg>0=CCW (anti-clockwise), angle_deg<0=CW. "
            "Calibrates gyro bias first (keep rover still). "
            "integrated_deg in response shows actual measured rotation.",
            tool_rotate_deg);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_FLOAT(t, "angle_deg");
        PROP_FLOAT(t, "speed");
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.drive_cm",
            "Drive straight forward by exact centimetres using calibrated time coefficient. "
            "Requires rover.set_nav_cal to be called first with k_dist value.",
            tool_drive_cm);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_FLOAT(t, "distance_cm");
        PROP_FLOAT(t, "speed");
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.nav_to",
            "Drive straight then rotate: first drives distance_cm (skip if 0), "
            "then rotates angle_deg (skip if 0). "
            "angle_deg>0=CCW, angle_deg<0=CW. Requires k_dist calibration for drive.",
            tool_nav_to);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_FLOAT(t, "distance_cm");
        PROP_FLOAT(t, "angle_deg");
        PROP_FLOAT(t, "speed");
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }
    TOOL("rover.get_nav_cal", "Get navigation calibration (k_dist, gyro_scale)", tool_get_nav_cal);
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.set_nav_cal",
            "Set navigation calibration and persist to NVS. "
            "k_dist = cm / (speed_unit * ms), e.g. 0.013 means 13 cm/s at speed=1.0. "
            "gyro_scale = physical_deg / integrated_deg (1.0 = no correction, 0 = keep current).",
            tool_set_nav_cal);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_FLOAT(t, "k_dist");
        PROP_FLOAT(t, "gyro_scale");
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }

    /* Power */
    TOOL("rover.get_power", "Get INA219 power status", tool_get_power);
    TOOL("rover.get_ups",   "Get UPS/battery status",  tool_get_ups);

    /* Power manager */
    TOOL("rover.power_get_status",
         "Get power manager status: current mode, battery voltage, idle time, active locks",
         tool_power_get_status);
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create(
            "rover.power_set_mode",
            "Set power mode (ACTIVE, IDLE, or LOW_POWER). "
            "Rejected if rover is moving or a sleep-prevention lock is active.",
            tool_power_set_mode);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_STR(t, "mode");
        PROP_STR(t, "reason");
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create(
            "rover.power_configure",
            "Update persistent power management settings. "
            "Integer/float fields: omit or pass 0 to leave unchanged. "
            "Boolean fields (wifi_power_save, reduce_cpu_frequency, disable_display_when_idle) "
            "are always applied — include all three when changing any. "
            "Most settings take effect after reboot.",
            tool_power_configure);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_INT_RANGE(t, "active_timeout_sec",     0, 3600);
        PROP_INT_RANGE(t, "idle_to_low_power_sec",  0, 86400);
        PROP_BOOL(t, "wifi_power_save");
        PROP_BOOL(t, "reduce_cpu_frequency");
        PROP_BOOL(t, "disable_display_when_idle");
        PROP_FLOAT(t, "critical_battery_voltage");
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create(
            "rover.power_prevent_sleep",
            "Acquire a sleep-prevention lock: while any lock is held the rover stays in ACTIVE mode. "
            "ttl_sec=0 means the lock never expires (must be released manually). "
            "Returns lock_id needed to release the lock.",
            tool_power_prevent_sleep);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_STR(t, "reason");
        PROP_INT_RANGE(t, "ttl_sec", 0, 604800);
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create(
            "rover.power_release_sleep_lock",
            "Release a sleep-prevention lock acquired with rover.power_prevent_sleep. "
            "Pass the lock_id returned by that call.",
            tool_power_release_sleep_lock);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_INT_RANGE(t, "lock_id", 0, 1000000);
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }

    /* IMU */
    TOOL("rover.get_imu", "Get IMU sensor data", tool_get_imu);
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.calibrate_imu",
            "Calibrate IMU gyro bias (keep rover still during calibration)",
            tool_calibrate_imu);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_INT_RANGE(t, "samples",     2, 500);
        PROP_INT_RANGE(t, "interval_ms", 1, 100);
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }

    /* Display */
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.display_text",
                                                 "Show text on OLED", tool_display_text);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_STR(t, "text");
        PROP_INT_RANGE(t, "line", 0, 3);
        PROP_BOOL(t, "clear");
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }
    TOOL("rover.display_clear",  "Clear OLED display",         tool_display_clear);
    TOOL("rover.display_status", "Show system status on OLED", tool_display_status);

    /* Wi-Fi */
    TOOL("rover.get_wifi", "Get Wi-Fi config (no passwords)", tool_get_wifi);
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.set_wifi",
                                                 "Set Wi-Fi config", tool_set_wifi);
        if (!t) return ESP_ERR_NO_MEM;
        PROP_STR(t, "ssid");
        PROP_STR(t, "password");
        PROP_STR(t, "mode");
        PROP_BOOL(t, "save");
        if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; }
    }

    ESP_LOGI(TAG, "registered all rover MCP tools");
    return ESP_OK;
}
