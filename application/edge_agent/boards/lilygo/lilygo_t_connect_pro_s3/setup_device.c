/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "dev_lcd_touch.h"
#include "esp_board_device.h"
#include "esp_board_manager_includes.h"
#include "esp_board_periph.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_cst226se.h"

static const char *TAG = "setup_device";

#define CST226SE_DEVICE_NAME       "lcd_touch"
#define CST226SE_I2C_NAME          "i2c_touch"
#define CST226SE_I2C_ADDR          0x5a
#define CST226SE_I2C_FREQ_HZ       400000

typedef struct {
    /* Must stay first: Board Manager exposes this member as the device handle. */
    dev_lcd_touch_handles_t handles;
    const char *i2c_name;
    bool owns_i2c_ref;
} cst226se_device_handles_t;

static esp_err_t cst226se_new_panel_io(i2c_master_bus_handle_t i2c_bus,
                                       const esp_lcd_panel_io_i2c_config_t *io_config,
                                       esp_lcd_panel_io_handle_t *ret_io)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    return esp_lcd_new_panel_io_i2c(i2c_bus, io_config, ret_io);
#else
    return esp_lcd_new_panel_io_i2c_v2(i2c_bus, io_config, ret_io);
#endif
}

static void cst226se_device_cleanup(cst226se_device_handles_t *touch)
{
    if (touch == NULL) {
        return;
    }
    if (touch->handles.touch_handle != NULL) {
        esp_lcd_touch_del(touch->handles.touch_handle);
        touch->handles.touch_handle = NULL;
    }
    if (touch->handles.io_handle != NULL) {
        esp_lcd_panel_io_del(touch->handles.io_handle);
        touch->handles.io_handle = NULL;
    }
    if (touch->owns_i2c_ref && touch->i2c_name != NULL) {
        esp_board_periph_unref_handle(touch->i2c_name);
        touch->owns_i2c_ref = false;
    }
    free(touch);
}

static int cst226se_device_init(void *cfg, int cfg_size, void **device_handle)
{
    if (cfg == NULL || cfg_size != sizeof(dev_lcd_touch_config_t) || device_handle == NULL) {
        ESP_LOGE(TAG, "Invalid CST226SE parameters");
        return -1;
    }

    const dev_lcd_touch_config_t *touch_cfg = (const dev_lcd_touch_config_t *)cfg;
    const char *i2c_name = touch_cfg->sub_cfg.i2c.i2c_name;
    if (i2c_name == NULL || i2c_name[0] == '\0') {
        i2c_name = CST226SE_I2C_NAME;
    }

    cst226se_device_handles_t *touch = calloc(1, sizeof(cst226se_device_handles_t));
    if (touch == NULL) {
        ESP_LOGE(TAG, "Allocate CST226SE handles failed");
        return -1;
    }

    void *i2c_bus = NULL;
    esp_err_t ret = esp_board_periph_ref_handle(i2c_name, &i2c_bus);
    if (ret != ESP_OK || i2c_bus == NULL) {
        ESP_LOGE(TAG, "Get CST226SE I2C bus failed: %s", esp_err_to_name(ret));
        cst226se_device_cleanup(touch);
        return -1;
    }
    touch->i2c_name = i2c_name;
    touch->owns_i2c_ref = true;

    esp_lcd_panel_io_i2c_config_t io_config = touch_cfg->sub_cfg.i2c.io_i2c_config;
    io_config.dev_addr = CST226SE_I2C_ADDR;
    if (io_config.scl_speed_hz == 0) {
        io_config.scl_speed_hz = CST226SE_I2C_FREQ_HZ;
    }

    ret = cst226se_new_panel_io((i2c_master_bus_handle_t)i2c_bus, &io_config,
                                &touch->handles.io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Create CST226SE panel IO failed: %s", esp_err_to_name(ret));
        cst226se_device_cleanup(touch);
        return -1;
    }

    ret = esp_lcd_touch_new_i2c_cst226se(touch->handles.io_handle,
                                         &touch_cfg->touch_config,
                                         &touch->handles.touch_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "New CST226SE touch failed: %s", esp_err_to_name(ret));
        cst226se_device_cleanup(touch);
        return -1;
    }

    ESP_LOGI(TAG, "CST226SE device initialized on %s at 7-bit address 0x%02x",
             i2c_name, CST226SE_I2C_ADDR);
    *device_handle = &touch->handles;
    return 0;
}

static int cst226se_device_deinit(void *device_handle)
{
    if (device_handle == NULL) {
        return -1;
    }
    cst226se_device_cleanup((cst226se_device_handles_t *)device_handle);
    return 0;
}

__attribute__((constructor))
static void register_cst226se_device_ops(void)
{
    esp_board_device_set_ops(CST226SE_DEVICE_NAME,
                             cst226se_device_init,
                             cst226se_device_deinit);
}



esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));
    panel_dev_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    int ret = esp_lcd_new_panel_st7789(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "New st7789 panel failed");
        return ret;
    }
    return ESP_OK;
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t ret = esp_lcd_touch_new_i2c_cst226se(io, touch_dev_config, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "New CST226SE touch failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
