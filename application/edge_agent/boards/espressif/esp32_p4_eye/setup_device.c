/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_sleep.h"

static const char *TAG = "P4_EYE_SETUP_DEVICE";

#define BSP_CAMERA_EN_PIN       (GPIO_NUM_12)
#define BSP_CAMERA_RST_PIN      (GPIO_NUM_26)
#define BSP_CAMERA_XCLK_PIN     (GPIO_NUM_11)
#define BSP_CAMERA_XCLK_FREQ    (24000000)
#define BSP_SD_EN_PIN           (GPIO_NUM_46)

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} st7789_lcd_init_cmd_t;

static const st7789_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0xB2, (uint8_t []){0x0C, 0x0C, 0x00, 0x33, 0x33}, 5, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x36, (uint8_t []){0x00}, 1, 0},
    {0x3A, (uint8_t []){0x05}, 1, 0},
    {0xB7, (uint8_t []){0x35}, 1, 0},
    {0xBB, (uint8_t []){0x2D}, 1, 0},
    {0xC0, (uint8_t []){0x2C}, 1, 0},
    {0xC2, (uint8_t []){0x01}, 1, 0},
    {0xC3, (uint8_t []){0x15}, 1, 0},
    {0xC4, (uint8_t []){0x20}, 1, 0},
    {0xC6, (uint8_t []){0x0F}, 1, 0},
    {0xD0, (uint8_t []){0xA4, 0xA1}, 2, 0},
    {0xD6, (uint8_t []){0xA1}, 1, 0},
    {0xE0, (uint8_t []){0x70, 0x05, 0x0A, 0x0B, 0x0A, 0x27, 0x2F, 0x44, 0x47, 0x37, 0x14, 0x14, 0x29, 0x2F}, 14, 0},
    {0xE1, (uint8_t []){0x70, 0x07, 0x0C, 0x08, 0x08, 0x04, 0x2F, 0x33, 0x46, 0x18, 0x15, 0x15, 0x2B, 0x2D}, 14, 0},
    {0x21, (uint8_t []){0x00}, 1, 0},
    {0x29, (uint8_t []){0x00}, 1, 0},
    {0x2C, (uint8_t []){0x00}, 1, 0},
};

static esp_cam_sensor_xclk_handle_t xclk_handle = NULL;

/*
 * Board early init: camera xclk, enable/reset GPIOs, SD card power.
 * Called via __attribute__((constructor)) so it runs before app_main / board_manager.
 */
static void __attribute__((constructor)) p4_eye_board_early_init(void)
{
    ESP_LOGI(TAG, "P4-EYE board early init");

    /* Camera XCLK via clock router */
    esp_cam_sensor_xclk_config_t cam_xclk_config = {
        .esp_clock_router_cfg = {
            .xclk_pin = BSP_CAMERA_XCLK_PIN,
            .xclk_freq_hz = BSP_CAMERA_XCLK_FREQ,
        }
    };
    ESP_ERROR_CHECK(esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &xclk_handle));
    ESP_ERROR_CHECK(esp_cam_sensor_xclk_start(xclk_handle, &cam_xclk_config));

    /* SD card enable pin (active low) */
    const gpio_config_t sdcard_io_config = {
        .pin_bit_mask = BIT64(BSP_SD_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&sdcard_io_config));
    gpio_set_level(BSP_SD_EN_PIN, 0);

    /* Camera reset pin */
    const gpio_config_t rst_io_config = {
        .pin_bit_mask = BIT64(BSP_CAMERA_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_io_config));
    gpio_set_level(BSP_CAMERA_RST_PIN, 1);

    /* Camera enable pin (RTC GPIO, active high) */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_ON);
    rtc_gpio_init(BSP_CAMERA_EN_PIN);
    rtc_gpio_set_direction(BSP_CAMERA_EN_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_pulldown_dis(BSP_CAMERA_EN_PIN);
    rtc_gpio_pullup_dis(BSP_CAMERA_EN_PIN);
    rtc_gpio_hold_dis(BSP_CAMERA_EN_PIN);
    rtc_gpio_set_level(BSP_CAMERA_EN_PIN, 1);
    rtc_gpio_hold_en(BSP_CAMERA_EN_PIN);
}

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = esp_lcd_new_panel_st7789(io, panel_dev_config, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %s", esp_err_to_name(ret));
        return ret;
    }

    for (size_t i = 0; i < sizeof(vendor_specific_init) / sizeof(vendor_specific_init[0]); i++) {
        ret = esp_lcd_panel_io_tx_param(io, vendor_specific_init[i].cmd,
                                        vendor_specific_init[i].data,
                                        vendor_specific_init[i].data_bytes);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send init cmd 0x%02x: %s", vendor_specific_init[i].cmd, esp_err_to_name(ret));
            return ret;
        }
        if (vendor_specific_init[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(vendor_specific_init[i].delay_ms));
        }
    }

    esp_lcd_panel_reset(*ret_panel);
    esp_lcd_panel_init(*ret_panel);

    return ESP_OK;
}
