/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal board support for M5Stack StickS3 without M5Unified.
 * Display is stubbed out (no M5GFX dependency). Voice + rover logic works.
 * TODO: Replace with proper M5Unified init once compatible IDF version is used.
 */
#include "setup_device.h"

extern "C" {
#include "driver/gpio.h"
#include "esp_log.h"
}

static const char *TAG = "setup_device";

/* StickS3 button GPIOs */
#define BTN_A_GPIO  GPIO_NUM_37
#define BTN_B_GPIO  GPIO_NUM_39

extern "C" esp_err_t rover_s3_board_init(void)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_A_GPIO) | (1ULL << BTN_B_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&btn_cfg);
    ESP_LOGI(TAG, "board_init done (display stubbed)");
    return err;
}

extern "C" void rover_s3_board_update(void) {}

extern "C" bool rover_s3_board_btn_a_pressed(void)
{
    return gpio_get_level(BTN_A_GPIO) == 0;
}

extern "C" bool rover_s3_board_btn_b_pressed(void)
{
    return gpio_get_level(BTN_B_GPIO) == 0;
}

extern "C" int rover_s3_board_get_battery_pct(void)
{
    return -1;  /* stub — needs AXP2101 I2C init */
}

extern "C" bool rover_s3_board_is_charging(void)
{
    return false;
}

extern "C" void rover_s3_board_display_state(rover_s3_display_state_t state,
                                               const char *ip, int batt_pct)
{
    /* Display stub — add ST7789 / M5Unified init here when IDF v5.5.x is used */
    static rover_s3_display_state_t last_state = (rover_s3_display_state_t)-1;
    if (state != last_state) {
        last_state = state;
        ESP_LOGI(TAG, "display_state=%d ip=%s batt=%d%%",
                 (int)state, ip ? ip : "--", batt_pct);
    }
}
