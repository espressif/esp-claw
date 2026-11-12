/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_touch_cst226se.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd_touch_cst226se";

#define CST226SE_REG_TOUCH_DATA          0x00
#define CST226SE_REG_DEVICE_ID           0x06
#define CST226SE_TOUCH_DATA_SIZE         26
#define CST226SE_MAX_POINTS              5
#define CST226SE_RAW_X_MAX               480
#define CST226SE_RAW_Y_MAX               320
#define CST226SE_DISPLAY_WIDTH           480
#define CST226SE_DISPLAY_HEIGHT          320
#define CST226SE_RESET_ASSERT_MS         100
#define CST226SE_RESET_READY_MS          1000

typedef struct {
    esp_lcd_panel_io_handle_t io;
    gpio_num_t reset_gpio;
    gpio_num_t interrupt_gpio;
} cst226se_touch_t;

static void cst226se_clear_points(esp_lcd_touch_handle_t tp)
{
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);
}

static esp_err_t cst226se_read_register(esp_lcd_panel_io_handle_t io,
                                        uint8_t reg,
                                        uint8_t *data,
                                        size_t data_len)
{
    return esp_lcd_panel_io_rx_param(io, reg, data, data_len);
}

static esp_err_t cst226se_reset(const esp_lcd_touch_config_t *config)
{
    if (config->rst_gpio_num == GPIO_NUM_NC) {
        return ESP_OK;
    }

    gpio_config_t reset_config = {
        .pin_bit_mask = BIT64(config->rst_gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&reset_config), TAG, "configure reset GPIO failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(config->rst_gpio_num, config->levels.reset),
                        TAG, "assert reset failed");
    vTaskDelay(pdMS_TO_TICKS(CST226SE_RESET_ASSERT_MS));
    ESP_RETURN_ON_ERROR(gpio_set_level(config->rst_gpio_num, !config->levels.reset),
                        TAG, "release reset failed");
    vTaskDelay(pdMS_TO_TICKS(CST226SE_RESET_READY_MS));
    return ESP_OK;
}

static esp_err_t cst226se_configure_interrupt_gpio(const esp_lcd_touch_config_t *config)
{
    if (config->int_gpio_num == GPIO_NUM_NC) {
        return ESP_OK;
    }

    bool active_low = config->levels.interrupt == 0;
    gpio_config_t interrupt_config = {
        .pin_bit_mask = BIT64(config->int_gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&interrupt_config);
}

static uint16_t cst226se_scale(uint16_t value, uint16_t input_max, uint16_t output_max)
{
    if (value > input_max) {
        value = input_max;
    }
    return (uint16_t)(((uint32_t)value * output_max) / input_max);
}

static esp_err_t cst226se_read_data(esp_lcd_touch_handle_t tp)
{
    cst226se_touch_t *driver = (cst226se_touch_t *)tp->config.driver_data;
    ESP_RETURN_ON_FALSE(driver != NULL, ESP_ERR_INVALID_STATE, TAG, "driver data is NULL");

    uint8_t data[CST226SE_TOUCH_DATA_SIZE] = {0};
    esp_err_t ret = cst226se_read_register(driver->io, CST226SE_REG_TOUCH_DATA,
                                            data, sizeof(data));
    if (ret != ESP_OK) {
        cst226se_clear_points(tp);
        return ret;
    }

    uint8_t point_count = data[5] & 0x0f;
    if (point_count == 0 || point_count > CST226SE_MAX_POINTS) {
        cst226se_clear_points(tp);
        return ESP_OK;
    }
    if (point_count > CONFIG_ESP_LCD_TOUCH_MAX_POINTS) {
        point_count = CONFIG_ESP_LCD_TOUCH_MAX_POINTS;
    }

    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = point_count;
    for (uint8_t i = 0; i < point_count; i++) {
        size_t coord_offset = i == 0 ? 1 : 8 + (i - 1) * 5;
        size_t pressure_offset = i == 0 ? 4 : coord_offset - 1;
        uint16_t raw_x = ((uint16_t)data[coord_offset] << 4) |
                         ((data[coord_offset + 2] & 0xf0) >> 4);
        uint16_t raw_y = ((uint16_t)data[coord_offset + 1] << 4) |
                         (data[coord_offset + 2] & 0x0f);

        uint16_t rotated_x = cst226se_scale(raw_y, CST226SE_RAW_Y_MAX,
                                             CST226SE_DISPLAY_WIDTH - 1);
        tp->data.coords[i].x = (CST226SE_DISPLAY_WIDTH - 1) - rotated_x;
        tp->data.coords[i].y = cst226se_scale(raw_x, CST226SE_RAW_X_MAX,
                                              CST226SE_DISPLAY_HEIGHT - 1);
        tp->data.coords[i].strength = data[pressure_offset];
        tp->data.coords[i].track_id = i;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static bool cst226se_get_xy(esp_lcd_touch_handle_t tp,
                            uint16_t *x,
                            uint16_t *y,
                            uint16_t *strength,
                            uint8_t *point_num,
                            uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    uint8_t count = tp->data.points < max_point_num ? tp->data.points : max_point_num;
    *point_num = count;
    for (uint8_t i = 0; i < count; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength != NULL) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return count > 0;
}

static esp_err_t cst226se_get_track_id(esp_lcd_touch_handle_t tp,
                                       uint8_t *track_id,
                                       uint8_t point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    uint8_t count = tp->data.points < point_num ? tp->data.points : point_num;
    for (uint8_t i = 0; i < count; i++) {
        track_id[i] = tp->data.coords[i].track_id;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static esp_err_t cst226se_del(esp_lcd_touch_handle_t tp)
{
    if (tp == NULL) {
        return ESP_OK;
    }

    cst226se_touch_t *driver = (cst226se_touch_t *)tp->config.driver_data;
    if (driver != NULL) {
        if (driver->reset_gpio != GPIO_NUM_NC) {
            gpio_reset_pin(driver->reset_gpio);
        }
        if (driver->interrupt_gpio != GPIO_NUM_NC) {
            gpio_reset_pin(driver->interrupt_gpio);
        }
        free(driver);
    }
    free(tp);
    return ESP_OK;
}

esp_err_t esp_lcd_touch_new_i2c_cst226se(esp_lcd_panel_io_handle_t io,
                                         const esp_lcd_touch_config_t *config,
                                         esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_FALSE(io != NULL && config != NULL && ret_touch != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid arguments");

    esp_lcd_touch_handle_t tp = calloc(1, sizeof(esp_lcd_touch_t));
    ESP_RETURN_ON_FALSE(tp != NULL, ESP_ERR_NO_MEM, TAG, "allocate touch handle failed");

    cst226se_touch_t *driver = calloc(1, sizeof(cst226se_touch_t));
    if (driver == NULL) {
        free(tp);
        return ESP_ERR_NO_MEM;
    }

    driver->io = io;
    driver->reset_gpio = config->rst_gpio_num;
    driver->interrupt_gpio = config->int_gpio_num;
    tp->config = *config;
    tp->config.x_max = CST226SE_DISPLAY_WIDTH;
    tp->config.y_max = CST226SE_DISPLAY_HEIGHT;
    tp->config.driver_data = driver;
    tp->io = io;
    tp->read_data = cst226se_read_data;
    tp->get_xy = cst226se_get_xy;
    tp->get_track_id = cst226se_get_track_id;
    tp->del = cst226se_del;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    tp->data.lock = lock;

    esp_err_t ret = cst226se_reset(config);
    if (ret == ESP_OK) {
        ret = cst226se_configure_interrupt_gpio(config);
    }
    uint8_t device_id = 0;
    if (ret == ESP_OK) {
        ret = cst226se_read_register(io, CST226SE_REG_DEVICE_ID,
                                     &device_id, sizeof(device_id));
    }
    if (ret != ESP_OK) {
        cst226se_del(tp);
        return ret;
    }

    ESP_LOGI(TAG, "CST226SE ready, device ID: 0x%02x", device_id);
    *ret_touch = tp;
    return ESP_OK;
}
