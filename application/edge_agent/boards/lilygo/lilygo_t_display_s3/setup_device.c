/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file setup_device.c
 * @brief LilyGO T-Display-S3 custom device initialization.
 *
 * The ST7789 LCD on T-Display-S3 is connected via an 8-bit parallel (Intel 8080
 * / i80) bus. This interface is not covered by esp_board_manager's built-in
 * display_lcd SPI/DSI paths, so we register a fully custom device here.
 *
 * Pin mapping (from T-Display-S3 schematic / pin_config.h):
 *   Power enable : GPIO15   (must be HIGH before using the LCD)
 *   Backlight    : GPIO38   (PWM via LEDC, active HIGH - controlled separately
 *                            by the lcd_brightness device)
 *   Reset        : GPIO5
 *   CS           : GPIO6
 *   DC           : GPIO7
 *   WR           : GPIO8
 *   RD           : GPIO9    (must be HIGH for write-only i80 mode)
 *   D0..D7       : GPIO39/40/41/42/45/46/47/48
 *   Resolution   : 320 x 170 (landscape)
 */

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gen_board_device_custom.h"

static const char *TAG = "lilygo_t_display_s3";

/* Pin definitions */
#define LCD_PIN_POWER_ON    15
#define LCD_PIN_RST          5
#define LCD_PIN_CS           6
#define LCD_PIN_DC           7
#define LCD_PIN_WR           8
#define LCD_PIN_RD           9
#define LCD_PIN_D0          39
#define LCD_PIN_D1          40
#define LCD_PIN_D2          41
#define LCD_PIN_D3          42
#define LCD_PIN_D4          45
#define LCD_PIN_D5          46
#define LCD_PIN_D6          47
#define LCD_PIN_D7          48

/* Display parameters */
#define LCD_H_RES           320
#define LCD_V_RES           170
/* ST7789 physical frame is 240 rows; 170-row panel is offset by 35 */
#define LCD_Y_GAP            35
/* Match LilyGO's working ESP-IDF reference timing for signal stability. */
#define LCD_PIXEL_CLK_HZ    (10 * 1000 * 1000)
#define LCD_CMD_BITS          8
#define LCD_PARAM_BITS        8
/* Keep DMA transactions modest to reduce i80 bus stress. */
#define LCD_MAX_TRANSFER_BYTES  (LCD_H_RES * 100 * sizeof(uint16_t))

typedef struct {
    uint8_t cmd;
    uint8_t data[14];
    uint8_t len;
} lcd_init_cmd_t;

static const lcd_init_cmd_t s_lcd_st7789v_cmds[] = {
    {0x11, {0}, 0 | 0x80}, /* SLPOUT */
    {0x3A, {0x05}, 1}, /* COLMOD: RGB565 */
    {0xB2, {0x0B, 0x0B, 0x00, 0x33, 0x33}, 5},
    {0xB7, {0x75}, 1},
    {0xBB, {0x28}, 1},
    {0xC0, {0x2C}, 1},
    {0xC2, {0x01}, 1},
    {0xC3, {0x1F}, 1},
    {0xC6, {0x13}, 1},
    {0xD0, {0xA7}, 1},
    {0xD0, {0xA4, 0xA1}, 2},
    {0xD6, {0xA1}, 1},
    {0xE0, {0xF0, 0x05, 0x0A, 0x06, 0x06, 0x03, 0x2B, 0x32, 0x43, 0x36, 0x11, 0x10, 0x2B, 0x32}, 14},
    {0xE1, {0xF0, 0x08, 0x0C, 0x0B, 0x09, 0x24, 0x2B, 0x22, 0x43, 0x38, 0x15, 0x16, 0x2F, 0x37}, 14},
};

static dev_display_lcd_handles_t s_lcd_handles;
static esp_lcd_i80_bus_handle_t s_i80_bus;

static const dev_display_lcd_config_t s_lcd_config = {
    .name = "display_lcd",
    .chip = "st7789",
    .sub_type = "i80",
    .lcd_width = LCD_H_RES,
    .lcd_height = LCD_V_RES,
    .swap_xy = 1,
    .mirror_x = 0,
    .mirror_y = 1,
    .need_reset = 1,
    .invert_color = 1,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    .bits_per_pixel = 16,
};

static void cleanup_display_lcd(esp_lcd_panel_handle_t panel_handle,
                                esp_lcd_panel_io_handle_t io_handle)
{
    if (panel_handle != NULL) {
        esp_lcd_panel_del(panel_handle);
    }
    if (io_handle != NULL) {
        esp_lcd_panel_io_del(io_handle);
    }
    if (s_i80_bus != NULL) {
        esp_lcd_del_i80_bus(s_i80_bus);
        s_i80_bus = NULL;
    }
}

static int display_lcd_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "device_handle is NULL");

    esp_err_t ret;

    gpio_config_t pwr_cfg = {
        .pin_bit_mask = BIT64(LCD_PIN_POWER_ON),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&pwr_cfg);
    ESP_RETURN_ON_ERROR(ret, TAG, "GPIO power-on config failed");
    gpio_set_level(LCD_PIN_POWER_ON, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_config_t rd_cfg = {
        .pin_bit_mask = BIT64(LCD_PIN_RD),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&rd_cfg);
    ESP_RETURN_ON_ERROR(ret, TAG, "GPIO RD config failed");
    gpio_set_level(LCD_PIN_RD, 1);

    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = LCD_PIN_DC,
        .wr_gpio_num = LCD_PIN_WR,
        .data_gpio_nums = {
            LCD_PIN_D0, LCD_PIN_D1, LCD_PIN_D2, LCD_PIN_D3,
            LCD_PIN_D4, LCD_PIN_D5, LCD_PIN_D6, LCD_PIN_D7,
        },
        .bus_width = 8,
        .max_transfer_bytes = LCD_MAX_TRANSFER_BYTES,
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    ret = esp_lcd_new_i80_bus(&bus_cfg, &i80_bus);
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_lcd_new_i80_bus failed");
    s_i80_bus = i80_bus;

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLK_HZ,
        .trans_queue_depth = 10,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
    };
    ret = esp_lcd_new_panel_io_i80(i80_bus, &io_cfg, &io_handle);
    if (ret != ESP_OK) {
        cleanup_display_lcd(NULL, NULL);
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_i80 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle);
    if (ret != ESP_OK) {
        cleanup_display_lcd(NULL, io_handle);
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_reset(panel_handle);
    if (ret != ESP_OK) {
        cleanup_display_lcd(panel_handle, io_handle);
        ESP_LOGE(TAG, "esp_lcd_panel_reset failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_init(panel_handle);
    if (ret != ESP_OK) {
        cleanup_display_lcd(panel_handle, io_handle);
        ESP_LOGE(TAG, "esp_lcd_panel_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_invert_color(panel_handle, true);
    if (ret != ESP_OK) {
        cleanup_display_lcd(panel_handle, io_handle);
        ESP_LOGE(TAG, "esp_lcd_panel_invert_color failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_swap_xy(panel_handle, true);
    if (ret != ESP_OK) {
        cleanup_display_lcd(panel_handle, io_handle);
        ESP_LOGE(TAG, "esp_lcd_panel_swap_xy failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_mirror(panel_handle, false, true);
    if (ret != ESP_OK) {
        cleanup_display_lcd(panel_handle, io_handle);
        ESP_LOGE(TAG, "esp_lcd_panel_mirror failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_set_gap(panel_handle, 0, LCD_Y_GAP);
    if (ret != ESP_OK) {
        cleanup_display_lcd(panel_handle, io_handle);
        ESP_LOGE(TAG, "esp_lcd_panel_set_gap failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (size_t i = 0; i < sizeof(s_lcd_st7789v_cmds) / sizeof(s_lcd_st7789v_cmds[0]); ++i) {
        ret = esp_lcd_panel_io_tx_param(io_handle, s_lcd_st7789v_cmds[i].cmd,
                                        s_lcd_st7789v_cmds[i].data,
                                        s_lcd_st7789v_cmds[i].len & 0x7F);
        if (ret != ESP_OK) {
            cleanup_display_lcd(panel_handle, io_handle);
            ESP_LOGE(TAG, "LCD init command 0x%02x failed: %s",
                     s_lcd_st7789v_cmds[i].cmd, esp_err_to_name(ret));
            return ret;
        }
        if (s_lcd_st7789v_cmds[i].len & 0x80) {
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }

    ret = esp_lcd_panel_disp_on_off(panel_handle, true);
    if (ret != ESP_OK) {
        cleanup_display_lcd(panel_handle, io_handle);
        ESP_LOGE(TAG, "esp_lcd_panel_disp_on_off failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_lcd_handles.io_handle = io_handle;
    s_lcd_handles.panel_handle = panel_handle;
    esp_board_device_override_config("display_lcd", (void *)&s_lcd_config, sizeof(s_lcd_config));
    *device_handle = &s_lcd_handles;

    ESP_LOGI(TAG, "T-Display-S3 i80 LCD ready (%dx%d @ %d Hz)",
             LCD_H_RES, LCD_V_RES, LCD_PIXEL_CLK_HZ);
    return ESP_OK;
}

static int display_lcd_deinit(void *device_handle)
{
    dev_display_lcd_handles_t *handles = (dev_display_lcd_handles_t *)device_handle;
    if (handles != NULL) {
        if (handles->panel_handle != NULL) {
            esp_lcd_panel_del(handles->panel_handle);
            handles->panel_handle = NULL;
        }
        if (handles->io_handle != NULL) {
            esp_lcd_panel_io_del(handles->io_handle);
            handles->io_handle = NULL;
        }
    }
    if (s_i80_bus != NULL) {
        esp_lcd_del_i80_bus(s_i80_bus);
        s_i80_bus = NULL;
    }
    gpio_set_level(LCD_PIN_POWER_ON, 0);
    ESP_LOGI(TAG, "T-Display-S3 i80 LCD deinitialized");
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(display_lcd, display_lcd_init, display_lcd_deinit);
