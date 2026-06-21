/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Prometheus text-exposition /metrics endpoint — surfaces power, motor,
 * IMU, and system-health telemetry already gathered by the HAL, for
 * scraping by Prometheus / Grafana Alloy.
 */
#include "wave_rover_mcp_metrics.h"
#include "wave_rover_hal.h"
#include "wave_rover_mcp_state.h"
#include "wave_rover_power_mgr.h"
#include "esp_netif.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"

static const char *TAG = "wr_metrics";
static const wave_rover_config_t *s_cfg       = NULL;
static wr_power_mgr_handle_t      s_power_mgr = NULL;

void wr_mcp_metrics_set_power_mgr(wr_power_mgr_handle_t pm) { s_power_mgr = pm; }

#define METRICS_BUF_SIZE 8192
#define FW_VERSION       "0.1.0"

/* ------------------------------------------------------------------ */
/* Auth — each local HTTP surface keeps its own small, self-contained  */
/* copy (see check_auth in wave_rover_mcp.c, web_check_auth in         */
/* wave_rover_mcp_web.c) rather than sharing a cross-file helper; this */
/* one follows the same established pattern.                           */
/* ------------------------------------------------------------------ */

/* Constant-time comparison to avoid timing side-channel on token */
static bool ct_streq(const char *a, const char *b)
{
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    volatile uint8_t diff = (uint8_t)(la ^ lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

#define AUTH_HDR_BUF 96

static bool metrics_check_auth(httpd_req_t *req)
{
    if (!s_cfg || !s_cfg->auth_enabled) return true;
    if (s_cfg->auth_token[0] == '\0')   return true;
    char buf[AUTH_HDR_BUF] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization",
                                    buf, sizeof(buf)) != ESP_OK) return false;
    if (strncmp(buf, "Bearer ", 7) != 0) return false;
    return ct_streq(buf + 7, s_cfg->auth_token);
}

#define METRICS_REQUIRE_AUTH(req) do { \
    if (!metrics_check_auth(req)) { \
        httpd_resp_set_status((req), "401 Unauthorized"); \
        httpd_resp_set_type((req), "application/json"); \
        return httpd_resp_sendstr((req), \
            "{\"ok\":false,\"error\":\"unauth\"}"); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Bounded text-buffer append                                          */
/* ------------------------------------------------------------------ */

/* Appends formatted text at *offset, clamping to buf_sz - 1 so the buffer
 * stays NUL-terminated and is never overrun. Truncates silently on
 * overflow rather than crashing — same bounded-write spirit as
 * wr_syslog.c's json_escape (best-effort output, never out of bounds). */
static void append(char *buf, size_t buf_sz, size_t *offset, const char *fmt, ...)
{
    if (*offset + 1 >= buf_sz) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *offset, buf_sz - *offset, fmt, args);
    va_end(args);
    if (n <= 0) return;
    size_t written = (size_t)n;
    size_t avail   = buf_sz - *offset - 1;
    *offset += (written < avail) ? written : avail;
}

static void append_help_type(char *buf, size_t sz, size_t *off,
                              const char *name, const char *type, const char *help)
{
    append(buf, sz, off, "# HELP %s %s\n", name, help);
    append(buf, sz, off, "# TYPE %s %s\n", name, type);
}

/* ------------------------------------------------------------------ */
/* Metric groups                                                       */
/* ------------------------------------------------------------------ */

static void append_power(char *buf, size_t sz, size_t *off)
{
    wr_power_status_t ps = {0};
    wr_power_get_status(&ps);

    append_help_type(buf, sz, off, "wave_rover_power_sensor_present", "gauge",
                     "Whether the INA219 power sensor is present (1) or absent (0)");
    append(buf, sz, off, "wave_rover_power_sensor_present %d\n", ps.present ? 1 : 0);
    if (!ps.present) return;

    append_help_type(buf, sz, off, "wave_rover_battery_voltage_volts", "gauge",
                     "Battery load voltage in volts");
    append(buf, sz, off, "wave_rover_battery_voltage_volts %.3f\n", (double)ps.load_voltage_v);

    append_help_type(buf, sz, off, "wave_rover_power_bus_voltage_volts", "gauge",
                     "INA219 bus voltage in volts");
    append(buf, sz, off, "wave_rover_power_bus_voltage_volts %.3f\n", (double)ps.bus_voltage_v);

    append_help_type(buf, sz, off, "wave_rover_power_shunt_voltage_millivolts", "gauge",
                     "INA219 shunt voltage in millivolts");
    append(buf, sz, off, "wave_rover_power_shunt_voltage_millivolts %.3f\n", (double)ps.shunt_voltage_mv);

    append_help_type(buf, sz, off, "wave_rover_power_current_milliamps", "gauge",
                     "Battery current draw in milliamps");
    append(buf, sz, off, "wave_rover_power_current_milliamps %.3f\n", (double)ps.current_ma);

    append_help_type(buf, sz, off, "wave_rover_power_draw_milliwatts", "gauge",
                     "Battery power draw in milliwatts");
    append(buf, sz, off, "wave_rover_power_draw_milliwatts %.3f\n", (double)ps.power_mw);

    append_help_type(buf, sz, off, "wave_rover_power_charging", "gauge",
                     "Whether the battery is currently charging (1) or not (0)");
    append(buf, sz, off, "wave_rover_power_charging %d\n", ps.charging ? 1 : 0);

    append_help_type(buf, sz, off, "wave_rover_power_low_battery", "gauge",
                     "Whether the low-battery threshold has been crossed (1) or not (0)");
    append(buf, sz, off, "wave_rover_power_low_battery %d\n", ps.low_battery ? 1 : 0);

    if (s_power_mgr) {
        wr_power_mode_t mode = wr_power_mgr_get_mode(s_power_mgr);
        append_help_type(buf, sz, off, "wave_rover_power_mode", "gauge",
                         "Current power mode — 1 for the active mode, 0 for the others");
        append(buf, sz, off, "wave_rover_power_mode{mode=\"active\"} %d\n",
               mode == WR_POWER_MODE_ACTIVE ? 1 : 0);
        append(buf, sz, off, "wave_rover_power_mode{mode=\"idle\"} %d\n",
               mode == WR_POWER_MODE_IDLE ? 1 : 0);
        append(buf, sz, off, "wave_rover_power_mode{mode=\"low_power\"} %d\n",
               mode == WR_POWER_MODE_LOW_POWER ? 1 : 0);

        char status_json[256];
        bool locks_active = false;
        if (wr_power_mgr_get_status_json(s_power_mgr, status_json, sizeof(status_json)) == ESP_OK) {
            locks_active = strstr(status_json, "\"locks_active\":true") != NULL;
        }
        append_help_type(buf, sz, off, "wave_rover_power_locks_active", "gauge",
                         "Whether any power-mode-floor locks are currently held (1) or not (0)");
        append(buf, sz, off, "wave_rover_power_locks_active %d\n", locks_active ? 1 : 0);
    }
}

static void append_motors(char *buf, size_t sz, size_t *off)
{
    wr_motor_state_t ms = {0};
    wr_motor_get_state(&ms);

    append_help_type(buf, sz, off, "wave_rover_motor_speed_ratio", "gauge",
                     "Commanded motor speed ratio, range -1.0 (full reverse) to 1.0 (full forward)");
    append(buf, sz, off, "wave_rover_motor_speed_ratio{side=\"left\"} %.3f\n", (double)ms.left);
    append(buf, sz, off, "wave_rover_motor_speed_ratio{side=\"right\"} %.3f\n", (double)ms.right);

    append_help_type(buf, sz, off, "wave_rover_emergency_stop", "gauge",
                     "Whether the emergency stop is currently active (1) or not (0)");
    append(buf, sz, off, "wave_rover_emergency_stop %d\n",
           wr_motor_emergency_stop_active() ? 1 : 0);

    append_help_type(buf, sz, off, "wave_rover_state", "gauge",
                     "Current rover state — 1 for the active state, 0 for the others");
    wr_rover_state_t cur = wr_rover_state_get();
    static const wr_rover_state_t all_states[] = {
        WR_ROVER_STATE_IDLE, WR_ROVER_STATE_DRIVING,
        WR_ROVER_STATE_NAV_BUSY, WR_ROVER_STATE_ESTOP,
    };
    for (size_t i = 0; i < sizeof(all_states) / sizeof(all_states[0]); i++) {
        append(buf, sz, off, "wave_rover_state{state=\"%s\"} %d\n",
               wr_rover_state_name(all_states[i]), all_states[i] == cur ? 1 : 0);
    }
}

static void append_imu(char *buf, size_t sz, size_t *off)
{
    wr_imu_sample_t s = {0};
    wr_imu_get_sample(&s);

    append_help_type(buf, sz, off, "wave_rover_imu_present", "gauge",
                     "Whether the IMU sensor is present (1) or absent (0)");
    append(buf, sz, off, "wave_rover_imu_present %d\n", s.present ? 1 : 0);
    if (!s.present) return;

    append_help_type(buf, sz, off, "wave_rover_imu_accel_g", "gauge",
                     "IMU linear acceleration in g, per axis");
    append(buf, sz, off, "wave_rover_imu_accel_g{axis=\"x\"} %.4f\n", (double)s.accel_x);
    append(buf, sz, off, "wave_rover_imu_accel_g{axis=\"y\"} %.4f\n", (double)s.accel_y);
    append(buf, sz, off, "wave_rover_imu_accel_g{axis=\"z\"} %.4f\n", (double)s.accel_z);

    append_help_type(buf, sz, off, "wave_rover_imu_gyro_dps", "gauge",
                     "IMU angular velocity in degrees per second, per axis");
    append(buf, sz, off, "wave_rover_imu_gyro_dps{axis=\"x\"} %.4f\n", (double)s.gyro_x);
    append(buf, sz, off, "wave_rover_imu_gyro_dps{axis=\"y\"} %.4f\n", (double)s.gyro_y);
    append(buf, sz, off, "wave_rover_imu_gyro_dps{axis=\"z\"} %.4f\n", (double)s.gyro_z);

    if (s.mag_present) {
        append_help_type(buf, sz, off, "wave_rover_imu_mag_microtesla", "gauge",
                         "IMU magnetometer reading in microtesla, per axis");
        append(buf, sz, off, "wave_rover_imu_mag_microtesla{axis=\"x\"} %.4f\n", (double)s.mag_x);
        append(buf, sz, off, "wave_rover_imu_mag_microtesla{axis=\"y\"} %.4f\n", (double)s.mag_y);
        append(buf, sz, off, "wave_rover_imu_mag_microtesla{axis=\"z\"} %.4f\n", (double)s.mag_z);
    }

    if (s.has_temperature) {
        append_help_type(buf, sz, off, "wave_rover_imu_temperature_celsius", "gauge",
                         "IMU die temperature in degrees Celsius");
        append(buf, sz, off, "wave_rover_imu_temperature_celsius %.2f\n", (double)s.temperature_c);
    }
}

static void append_system(char *buf, size_t sz, size_t *off)
{
    append_help_type(buf, sz, off, "wave_rover_uptime_seconds", "gauge",
                     "Seconds since boot");
    append(buf, sz, off, "wave_rover_uptime_seconds %lld\n",
           (long long)(esp_timer_get_time() / 1000000));

    append_help_type(buf, sz, off, "wave_rover_free_heap_bytes", "gauge",
                     "Current free heap size in bytes");
    append(buf, sz, off, "wave_rover_free_heap_bytes %lu\n",
           (unsigned long)esp_get_free_heap_size());

    append_help_type(buf, sz, off, "wave_rover_min_free_heap_bytes", "gauge",
                     "Minimum free heap size observed since boot, in bytes");
    append(buf, sz, off, "wave_rover_min_free_heap_bytes %lu\n",
           (unsigned long)esp_get_minimum_free_heap_size());

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool connected = (sta_netif != NULL &&
                      esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK &&
                      ip_info.ip.addr != 0);

    append_help_type(buf, sz, off, "wave_rover_wifi_connected", "gauge",
                     "Whether the rover is connected to a Wi-Fi AP (1) or not (0)");
    append(buf, sz, off, "wave_rover_wifi_connected %d\n", connected ? 1 : 0);

    if (connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            append_help_type(buf, sz, off, "wave_rover_wifi_rssi_dbm", "gauge",
                             "Wi-Fi signal strength of the connected AP, in dBm");
            append(buf, sz, off, "wave_rover_wifi_rssi_dbm %d\n", (int)ap_info.rssi);
        }
    }

    append_help_type(buf, sz, off, "wave_rover_build_info", "gauge",
                     "Always 1; firmware build metadata is carried in labels");
    append(buf, sz, off, "wave_rover_build_info{version=\"%s\"} 1\n", FW_VERSION);
}

/* ------------------------------------------------------------------ */
/* HTTP handler                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t handle_metrics(httpd_req_t *req)
{
    METRICS_REQUIRE_AUTH(req);

    char *buf = malloc(METRICS_BUF_SIZE);
    if (!buf) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
    }

    size_t off = 0;
    buf[0] = '\0';
    append_power(buf, METRICS_BUF_SIZE, &off);
    append_motors(buf, METRICS_BUF_SIZE, &off);
    append_imu(buf, METRICS_BUF_SIZE, &off);
    append_system(buf, METRICS_BUF_SIZE, &off);

    httpd_resp_set_type(req, "text/plain; version=0.0.4");
    esp_err_t err = httpd_resp_send(req, buf, off);
    free(buf);
    return err;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t wr_mcp_metrics_register(httpd_handle_t server, const wave_rover_config_t *cfg)
{
    s_cfg = cfg;

    httpd_uri_t uri = {
        .uri      = "/metrics",
        .method   = HTTP_GET,
        .handler  = handle_metrics,
        .user_ctx = NULL,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to register /metrics: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "metrics endpoint registered at /metrics");
    return ESP_OK;
}
