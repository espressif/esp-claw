/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include "M5Unified.h"

extern "C" {
#include "setup_device.h"
}

static rover_s3_display_state_t s_last_state = (rover_s3_display_state_t)-1;
static char s_last_ip[20] = {0};
static int  s_last_batt = -2;

static const char *STATE_LABELS[] = {
    "BOOT", "IDLE", "LISTEN", "THINK", "SPEAK", "EXEC", "OFFLINE"
};
static uint32_t STATE_COLORS[] = {
    TFT_DARKGREY, TFT_GREEN, TFT_CYAN, TFT_YELLOW,
    TFT_MAGENTA, TFT_ORANGE, TFT_RED
};

extern "C" esp_err_t rover_s3_board_init(void)
{
    auto cfg = M5.config();
    cfg.output_power = true;
    cfg.internal_imu = true;
    cfg.internal_rtc = true;
    cfg.internal_mic = false;
    cfg.internal_spk = false;
    cfg.led_brightness = 0;
    M5.begin(cfg);
    M5.Display.setRotation(1);
    M5.Display.setBrightness(128);
    M5.Display.fillScreen(TFT_BLACK);
    return ESP_OK;
}

extern "C" void rover_s3_board_update(void)
{
    M5.update();
}

extern "C" bool rover_s3_board_btn_a_pressed(void)
{
    return M5.BtnA.isPressed();
}

extern "C" bool rover_s3_board_btn_b_pressed(void)
{
    return M5.BtnB.isPressed();
}

extern "C" int rover_s3_board_get_battery_pct(void)
{
    int pct = M5.Power.getBatteryLevel();
    return (pct < 0) ? -1 : (pct > 100 ? 100 : pct);
}

extern "C" bool rover_s3_board_is_charging(void)
{
    return M5.Power.isCharging();
}

extern "C" void rover_s3_board_display_state(rover_s3_display_state_t state,
                                               const char *ip, int batt_pct)
{
    bool changed = (state != s_last_state)
                || strcmp(ip ? ip : "", s_last_ip) != 0
                || (batt_pct != s_last_batt);
    if (!changed) return;

    s_last_state = state;
    strlcpy(s_last_ip, ip ? ip : "", sizeof(s_last_ip));
    s_last_batt = batt_pct;

    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(STATE_COLORS[state], TFT_BLACK);
    M5.Display.setCursor(4, 4);
    M5.Display.print(STATE_LABELS[state]);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(4, 40);
    M5.Display.printf("IP: %s", s_last_ip[0] ? s_last_ip : "--");
    M5.Display.setCursor(4, 56);
    M5.Display.printf("BAT: %d%%", batt_pct);
}
