/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_touch_hi8561.h"
#include "esp_log.h"

static const char *TAG = "esp_lcd_touch_hi8561";

#define HI8561_TOUCH_WIDTH                540
#define HI8561_TOUCH_HEIGHT               1168
#define HI8561_MEMORY_ERAM                0x20011000u
#define HI8561_MEMORY_ERAM_SIZE           (4 * 1024)
#define HI8561_ESRAM_INFO_REG             0x200110d0u
#define HI8561_TOUCH_OFFSET               3
#define HI8561_TOUCH_SIZE                 5
#define HI8561_MAX_POINTS                 10
#define HI8561_I2C_TIMEOUT_MS             200

typedef struct {
    i2c_master_dev_handle_t i2c_dev;
    uint32_t touch_info_addr;
} hi8561_touch_t;

static void write_u32_be(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value >> 24);
    buf[1] = (uint8_t)(value >> 16);
    buf[2] = (uint8_t)(value >> 8);
    buf[3] = (uint8_t)value;
}

static esp_err_t hi8561_read_mem(i2c_master_dev_handle_t i2c_dev,
                                 uint32_t addr,
                                 uint8_t *data,
                                 size_t data_len)
{
    uint8_t reg_buf[6] = {0xf3, 0, 0, 0, 0, 0x03};
    write_u32_be(&reg_buf[1], addr);
    return i2c_master_transmit_receive(i2c_dev, reg_buf, sizeof(reg_buf),
                                       data, data_len, HI8561_I2C_TIMEOUT_MS);
}

static esp_err_t hi8561_init_address_info(hi8561_touch_t *driver)
{
    uint8_t info[48] = {0};
    ESP_RETURN_ON_ERROR(hi8561_read_mem(driver->i2c_dev, HI8561_ESRAM_INFO_REG,
                                        info, sizeof(info)),
                        TAG, "read touch info address failed");

    uint32_t addr = (uint32_t)info[8] | ((uint32_t)info[9] << 8) |
                    ((uint32_t)info[10] << 16) | ((uint32_t)info[11] << 24);
    ESP_RETURN_ON_FALSE(addr >= HI8561_MEMORY_ERAM &&
                            addr < (HI8561_MEMORY_ERAM + HI8561_MEMORY_ERAM_SIZE),
                        ESP_FAIL, TAG, "invalid touch info address: 0x%08lx", (unsigned long)addr);
    driver->touch_info_addr = addr;
    return ESP_OK;
}

static void clear_points(esp_lcd_touch_handle_t tp)
{
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);
}

static esp_err_t hi8561_read_data(esp_lcd_touch_handle_t tp)
{
    hi8561_touch_t *driver = (hi8561_touch_t *)tp->config.driver_data;
    ESP_RETURN_ON_FALSE(driver != NULL, ESP_ERR_INVALID_STATE, TAG, "driver data is NULL");

    ESP_RETURN_ON_FALSE(driver->touch_info_addr != 0, ESP_ERR_INVALID_STATE,
                        TAG, "touch info address is not initialized");

    uint8_t buf[HI8561_TOUCH_OFFSET + HI8561_MAX_POINTS * HI8561_TOUCH_SIZE] = {0};
    ESP_RETURN_ON_ERROR(hi8561_read_mem(driver->i2c_dev, driver->touch_info_addr,
                                        buf, sizeof(buf)),
                        TAG, "read touch points failed");

    uint8_t count = buf[0];
    if (count == 0 || count > HI8561_MAX_POINTS) {
        clear_points(tp);
        return ESP_OK;
    }
    if (count > CONFIG_ESP_LCD_TOUCH_MAX_POINTS) {
        count = CONFIG_ESP_LCD_TOUCH_MAX_POINTS;
    }

    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    for (uint8_t i = 0; i < count; i++) {
        size_t offset = HI8561_TOUCH_OFFSET + i * HI8561_TOUCH_SIZE;
        uint16_t x = ((uint16_t)buf[offset] << 8) | buf[offset + 1];
        uint16_t y = ((uint16_t)buf[offset + 2] << 8) | buf[offset + 3];
        uint8_t strength = buf[offset + 4];
        if (x == 0xffff && y == 0xffff && strength == 0) {
            continue;
        }
        tp->data.coords[tp->data.points].x = x;
        tp->data.coords[tp->data.points].y = y;
        tp->data.coords[tp->data.points].strength = strength;
        tp->data.coords[tp->data.points].track_id = i;
        tp->data.points++;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static bool hi8561_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                          uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
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

static esp_err_t hi8561_get_track_id(esp_lcd_touch_handle_t tp, uint8_t *track_id, uint8_t point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    uint8_t count = tp->data.points < point_num ? tp->data.points : point_num;
    for (uint8_t i = 0; i < count; i++) {
        track_id[i] = tp->data.coords[i].track_id;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static esp_err_t hi8561_del(esp_lcd_touch_handle_t tp)
{
    if (tp == NULL) {
        return ESP_OK;
    }
    hi8561_touch_t *driver = (hi8561_touch_t *)tp->config.driver_data;
    if (driver != NULL) {
        free(driver);
    }
    free(tp);
    return ESP_OK;
}

esp_err_t esp_lcd_touch_new_i2c_hi8561(i2c_master_dev_handle_t i2c_dev,
                                       const esp_lcd_touch_config_t *config,
                                       esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_FALSE(i2c_dev != NULL && config != NULL && ret_touch != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid arguments");

    esp_lcd_touch_handle_t tp = calloc(1, sizeof(esp_lcd_touch_t));
    ESP_RETURN_ON_FALSE(tp != NULL, ESP_ERR_NO_MEM, TAG, "alloc touch handle failed");

    hi8561_touch_t *driver = calloc(1, sizeof(hi8561_touch_t));
    if (driver == NULL) {
        free(tp);
        return ESP_ERR_NO_MEM;
    }

    driver->i2c_dev = i2c_dev;

    tp->config = *config;
    tp->config.x_max = HI8561_TOUCH_WIDTH;
    tp->config.y_max = HI8561_TOUCH_HEIGHT;
    tp->config.driver_data = driver;
    tp->read_data = hi8561_read_data;
    tp->get_xy = hi8561_get_xy;
    tp->get_track_id = hi8561_get_track_id;
    tp->del = hi8561_del;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    tp->data.lock = lock;

    esp_err_t ret = hi8561_init_address_info(driver);
    if (ret != ESP_OK) {
        hi8561_del(tp);
        return ret;
    }
    ESP_LOGI(TAG, "HI8561 touch info address: 0x%08lx",
             (unsigned long)driver->touch_info_addr);

    *ret_touch = tp;
    return ESP_OK;
}
