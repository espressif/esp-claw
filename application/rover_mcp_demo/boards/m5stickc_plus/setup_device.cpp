/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "M5Unified.h"

extern "C" {
#include "setup_device.h"
}

static bool s_imu_ok = false;
static rover_display_state_t s_last_state = (rover_display_state_t)-1;
static char s_last_ip[20] = {0};
static int s_last_batt = -2;
static bool s_last_charging = false;

extern "C" esp_err_t rover_board_init(void)
{
    auto cfg = M5.config();
    cfg.output_power = true;
    cfg.internal_imu = true;
    cfg.internal_rtc = true;
    cfg.internal_mic = false;
    cfg.internal_spk = false;
    cfg.led_brightness = 0;

    M5.begin(cfg);
    M5.Display.setRotation(3);
    M5.Display.setBrightness(128);
    M5.Display.fillScreen(TFT_BLACK);
    s_imu_ok = M5.Imu.isEnabled();
    return ESP_OK;
}

extern "C" void rover_board_m5_update(void)
{
    M5.update();
}

extern "C" bool rover_board_btn_a_pressed(void)
{
    return M5.BtnA.isPressed();
}

extern "C" bool rover_board_btn_b_pressed(void)
{
    return M5.BtnB.isPressed();
}

extern "C" int rover_board_get_battery_pct(void)
{
    int pct = M5.Power.getBatteryLevel();
    if (pct < 0) {
        return -1;
    }
    return pct > 100 ? 100 : pct;
}

extern "C" void rover_board_get_power_status(rover_power_status_t *out)
{
    if (!out) {
        return;
    }
    int pct = M5.Power.getBatteryLevel();
    out->battery_pct = (pct < 0) ? -1 : (pct > 100 ? 100 : pct);
    out->charging = (M5.Power.isCharging() == m5::Power_Class::is_charging);
    int mv = M5.Power.getBatteryVoltage();
    out->battery_mv = (mv > 0) ? mv : 0;
}

extern "C" bool rover_board_imu_enabled(void)
{
    return s_imu_ok;
}

extern "C" esp_err_t rover_board_imu_read(float *ax, float *ay, float *az,
                                          float *gx, float *gy, float *gz)
{
    if (!s_imu_ok) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    bool a_ok = M5.Imu.getAccel(ax, ay, az);
    bool g_ok = M5.Imu.getGyro(gx, gy, gz);
    return a_ok && g_ok ? ESP_OK : ESP_FAIL;
}

static const char *state_label(rover_display_state_t s)
{
    switch (s) {
    case ROVER_DISPLAY_STATE_BOOT:
        return "BOOT";
    case ROVER_DISPLAY_STATE_IDLE:
        return "IDLE";
    case ROVER_DISPLAY_STATE_THINKING:
        return "AI_THINK";
    case ROVER_DISPLAY_STATE_EXECUTING:
        return "AI_EXEC";
    case ROVER_DISPLAY_STATE_OFFLINE:
        return "OFFLINE";
    case ROVER_DISPLAY_STATE_SLEEPING:
        return "SLEEP";
    default:
        return "?";
    }
}

static uint32_t state_color(rover_display_state_t s)
{
    switch (s) {
    case ROVER_DISPLAY_STATE_IDLE:
        return 0x2563EBu;
    case ROVER_DISPLAY_STATE_THINKING:
    case ROVER_DISPLAY_STATE_EXECUTING:
        return 0xEA580Cu;
    case ROVER_DISPLAY_STATE_OFFLINE:
        return 0xDC2626u;
    case ROVER_DISPLAY_STATE_SLEEPING:
        return 0x6B21A8u;
    default:
        return 0x111827u;
    }
}

extern "C" void rover_board_display_render(rover_display_state_t state,
                                           const char *ip,
                                           const rover_power_status_t *power)
{
    const char *ip_str = ip ? ip : "";
    int battery_pct = power ? power->battery_pct : -1;
    bool charging = power ? power->charging : false;

    if (state == s_last_state && strcmp(ip_str, s_last_ip) == 0 &&
            battery_pct == s_last_batt && charging == s_last_charging) {
        return;
    }

    s_last_state = state;
    strlcpy(s_last_ip, ip_str, sizeof(s_last_ip));
    s_last_batt = battery_pct;
    s_last_charging = charging;

    const uint32_t bg = 0x111827u;
    const uint32_t bar = state_color(state);
    M5.Display.startWrite();
    M5.Display.fillScreen(bg);
    M5.Display.fillRoundRect(2, 2, 236, 24, 4, bar);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_WHITE, bar);
    M5.Display.setCursor(8, 6);
    M5.Display.print("AI Rover");

    const char *label = state_label(state);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(TFT_WHITE, bg);
    int label_w = (int)strlen(label) * 18;
    M5.Display.setCursor((240 - label_w) / 2, 50);
    M5.Display.print(label);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x9CA3AFu, bg);
    M5.Display.setCursor(8, 116);
    M5.Display.print(ip_str[0] ? ip_str : "no wifi");

    if (battery_pct >= 0) {
        char batt_buf[16] = {0};
        if (charging) {
            snprintf(batt_buf, sizeof(batt_buf), "CHG %d%%", battery_pct);
        } else {
            snprintf(batt_buf, sizeof(batt_buf), "%d%%", battery_pct);
        }
        uint32_t batt_color = charging ? 0x34D399u : 0x9CA3AFu;
        int batt_w = (int)strlen(batt_buf) * 6;
        M5.Display.setTextColor(batt_color, bg);
        M5.Display.setCursor(240 - batt_w - 8, 116);
        M5.Display.print(batt_buf);
    }
    M5.Display.endWrite();
}

extern "C" void rover_board_display_sleep(void)
{
    M5.Display.setBrightness(0);
    M5.Display.sleep();
}

extern "C" void rover_board_display_set_brightness(uint8_t brightness)
{
    M5.Display.setBrightness(brightness);
}
