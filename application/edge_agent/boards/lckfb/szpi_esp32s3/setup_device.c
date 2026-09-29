/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Board setup for LCKFB SZPI ESP32-S3 (立创·实战派 ESP32-S3).
 *
 * This board uses a PCA9557 I2C IO expander to drive three signals that have
 * no dedicated SoC GPIO:
 *   - LCD_CS   (PCA9557 IO0)
 *   - PA_EN    (PCA9557 IO1)  speaker amplifier enable
 *   - DVP_PWDN (PCA9557 IO2)  camera power-down (low = powered)
 *
 * The expander is exposed as a `custom` board device (`pca9557`).
 *
 * LCD CS timing mirrors the board's proven bring-up sequence: the expander is
 * initialized with LCD_CS de-asserted (high), then the ST7789 panel factory
 * asserts LCD_CS (low) right before the controller init sequence runs. The
 * `display_lcd` device uses `need_reset: false` so the board performs the
 * reset/select ordering itself.
 */

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_board_manager_includes.h"

#include "gen_board_device_custom.h"

static const char *TAG = "SZPI_ESP32S3_SETUP";

/* PCA9557 register map (identical to TCA9554) */
#define PCA9557_REG_OUTPUT_PORT 0x01
#define PCA9557_REG_CONFIG_PORT 0x03

/* IO0 = LCD_CS, IO1 = PA_EN, IO2 = DVP_PWDN (low = powered) */
#define PCA9557_OUT_DVP_PWDN_BIT 2
#define PCA9557_OUT_PA_EN_BIT    1
#define PCA9557_OUT_LCD_CS_BIT   0

/* LCD_CS de-asserted (high), PA_EN on, camera powered */
#define PCA9557_OUTPUT_INIT_VALUE 0x03
/* LCD_CS asserted (low), PA_EN on, camera powered */
#define PCA9557_OUTPUT_LCD_SELECTED 0x02
/* IO0..IO2 as outputs, IO3..IO7 stay inputs */
#define PCA9557_CONFIG_VALUE 0xF8

static i2c_master_dev_handle_t s_pca9557_dev;

static esp_err_t szpi_pca9557_write_output(uint8_t value)
{
    const uint8_t buf[2] = { PCA9557_REG_OUTPUT_PORT, value };

    ESP_RETURN_ON_FALSE(s_pca9557_dev != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "PCA9557 not initialized");
    return i2c_master_transmit(s_pca9557_dev, buf, sizeof(buf), 1000);
}

static int pca9557_init(void *config, int cfg_size, void **device_handle)
{
    dev_custom_pca9557_config_t *cfg = (dev_custom_pca9557_config_t *)config;
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_dev_handle_t i2c_dev = NULL;
    const uint8_t output_reg[2] = { PCA9557_REG_OUTPUT_PORT, PCA9557_OUTPUT_INIT_VALUE };
    const uint8_t config_reg[2] = { PCA9557_REG_CONFIG_PORT, PCA9557_CONFIG_VALUE };

    (void)cfg_size;
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "pca9557 config is NULL");

    ESP_RETURN_ON_ERROR(esp_board_periph_get_handle(cfg->peripheral_name, (void **)&i2c_bus),
                        TAG, "Failed to get I2C bus '%s'", cfg->peripheral_name);

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->i2c_addr,
        .scl_speed_hz = cfg->frequency,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &i2c_dev),
                        TAG, "Failed to add PCA9557 device");

    ESP_RETURN_ON_ERROR(i2c_master_transmit(i2c_dev, output_reg, sizeof(output_reg), 1000),
                        TAG, "Failed to set PCA9557 outputs");
    ESP_RETURN_ON_ERROR(i2c_master_transmit(i2c_dev, config_reg, sizeof(config_reg), 1000),
                        TAG, "Failed to set PCA9557 directions");

    s_pca9557_dev = i2c_dev;
    ESP_LOGI(TAG, "PCA9557 @0x%02x ready: LCD_CS=1 (deselected), PA_EN=1, DVP_PWDN=0",
             (unsigned)cfg->i2c_addr);
    *device_handle = i2c_dev;
    return ESP_OK;
}

static int pca9557_deinit(void *device_handle)
{
    if (device_handle != NULL) {
        esp_err_t err = i2c_master_bus_rm_device((i2c_master_dev_handle_t)device_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to remove PCA9557 device: %s", esp_err_to_name(err));
        }
    }
    s_pca9557_dev = NULL;
    return ESP_OK;
}

/* First argument must match the `name` field in board_devices.yaml. */
CUSTOM_DEVICE_IMPLEMENT(pca9557, pca9557_init, pca9557_deinit);

/*
 * ST7789 LCD panel factory. The SPI CS line lives on the PCA9557 expander, so
 * the panel relies on the externally driven CS instead of an esp_lcd CS GPIO.
 *
 * Ordering matches the board's reference bring-up:
 *   1. create the panel driver object
 *   2. issue SWRESET while LCD_CS is still de-asserted (ignored by the panel,
 *      exactly like the reference sequence which resets before selecting CS)
 *   3. assert LCD_CS (low)
 * Then BMGR runs the controller init sequence with CS asserted because the
 * `display_lcd` device sets `need_reset: false`.
 */
#if __has_include("esp_lcd_panel_st7789.h")
#include "esp_lcd_panel_st7789.h"

#define ST7789_CMD_SWRESET 0x01

__attribute__((weak)) esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                                          const esp_lcd_panel_dev_config_t *panel_dev_config,
                                                          esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t err = esp_lcd_new_panel_st7789(io, panel_dev_config, ret_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %s", esp_err_to_name(err));
        return err;
    }

    /* Reference ordering: reset while CS is de-asserted, then select the panel. */
    (void)esp_lcd_panel_io_tx_param(io, ST7789_CMD_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    err = szpi_pca9557_write_output(PCA9557_OUTPUT_LCD_SELECTED);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to assert LCD_CS: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}
#endif /* __has_include("esp_lcd_panel_st7789.h") */

/* FT6336 (FT5x06-compatible) capacitive touch factory. */
#if __has_include("esp_lcd_touch_ft5x06.h")
#include "esp_lcd_touch_ft5x06.h"

__attribute__((weak)) esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                                          const esp_lcd_touch_config_t *touch_dev_config,
                                                          esp_lcd_touch_handle_t *ret_touch)
{
    return esp_lcd_touch_new_i2c_ft5x06(io, touch_dev_config, ret_touch);
}
#endif /* __has_include("esp_lcd_touch_ft5x06.h") */
