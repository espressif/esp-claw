/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "esp_check.h"
#include "esp_lcd_touch_gt9895.h"
#include "esp_log.h"

static const char *TAG = "esp_lcd_touch_gt9895";

#define GT9895_TOUCH_WIDTH             568
#define GT9895_TOUCH_HEIGHT            1232
#define GT9895_ID_REG                  0x00010070u
#define GT9895_TOUCH_REG               0x00010308u
#define GT9895_ID                      0xadu
#define GT9895_TOUCH_OFFSET            8
#define GT9895_TOUCH_SIZE              8
#define GT9895_MAX_POINTS              10

typedef struct {
    esp_lcd_panel_io_handle_t io;
} gt9895_touch_t;

static void write_u32_be(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value >> 24);
    buf[1] = (uint8_t)(value >> 16);
    buf[2] = (uint8_t)(value >> 8);
    buf[3] = (uint8_t)value;
}

static esp_err_t gt9895_read_reg(esp_lcd_panel_io_handle_t io,
                                 uint32_t reg,
                                 uint8_t *data,
                                 size_t data_len)
{
    uint8_t reg_buf[4];
    write_u32_be(reg_buf, reg);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, -1, reg_buf, sizeof(reg_buf)),
                        TAG, "write touch register failed");
    return esp_lcd_panel_io_rx_param(io, -1, data, data_len);
}

esp_err_t esp_lcd_touch_gt9895_check_id(esp_lcd_panel_io_handle_t io)
{
    uint8_t id = 0;
    ESP_RETURN_ON_FALSE(io != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid panel io");
    ESP_RETURN_ON_ERROR(gt9895_read_reg(io, GT9895_ID_REG, &id, sizeof(id)),
                        TAG, "read touch id failed");
    ESP_RETURN_ON_FALSE(id == GT9895_ID, ESP_FAIL, TAG, "unexpected touch id: 0x%02x", id);
    return ESP_OK;
}

static void clear_points(esp_lcd_touch_handle_t tp)
{
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);
}

static esp_err_t gt9895_read_data(esp_lcd_touch_handle_t tp)
{
    gt9895_touch_t *driver = (gt9895_touch_t *)tp->config.driver_data;
    ESP_RETURN_ON_FALSE(driver != NULL, ESP_ERR_INVALID_STATE, TAG, "driver data is NULL");

    uint8_t buf[GT9895_TOUCH_OFFSET + GT9895_MAX_POINTS * GT9895_TOUCH_SIZE] = {0};
    ESP_RETURN_ON_ERROR(gt9895_read_reg(driver->io, GT9895_TOUCH_REG, buf, sizeof(buf)),
                        TAG, "read touch points failed");

    uint8_t count = buf[2];
    if (count == 0 || count > GT9895_MAX_POINTS) {
        clear_points(tp);
        return ESP_OK;
    }
    if (count > CONFIG_ESP_LCD_TOUCH_MAX_POINTS) {
        count = CONFIG_ESP_LCD_TOUCH_MAX_POINTS;
    }

    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = count;
    for (uint8_t i = 0; i < count; i++) {
        size_t offset = GT9895_TOUCH_OFFSET + i * GT9895_TOUCH_SIZE;
        uint16_t raw_x = (uint16_t)buf[offset + 2] | ((uint16_t)buf[offset + 3] << 8);
        uint16_t raw_y = (uint16_t)buf[offset + 4] | ((uint16_t)buf[offset + 5] << 8);
        tp->data.coords[i].x = (uint16_t)(raw_x * GT9895_TOUCH_WIDTH / 1060);
        tp->data.coords[i].y = (uint16_t)(raw_y * GT9895_TOUCH_HEIGHT / 2400);
        tp->data.coords[i].strength = buf[offset + 6];
        tp->data.coords[i].track_id = (buf[offset] >> 4) + 1;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static bool gt9895_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
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

static esp_err_t gt9895_get_track_id(esp_lcd_touch_handle_t tp, uint8_t *track_id, uint8_t point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    uint8_t count = tp->data.points < point_num ? tp->data.points : point_num;
    for (uint8_t i = 0; i < count; i++) {
        track_id[i] = tp->data.coords[i].track_id;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static esp_err_t gt9895_del(esp_lcd_touch_handle_t tp)
{
    if (tp == NULL) {
        return ESP_OK;
    }
    gt9895_touch_t *driver = (gt9895_touch_t *)tp->config.driver_data;
    if (driver != NULL) {
        free(driver);
    }
    free(tp);
    return ESP_OK;
}

esp_err_t esp_lcd_touch_new_i2c_gt9895(esp_lcd_panel_io_handle_t io,
                                       const esp_lcd_touch_config_t *config,
                                       esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_FALSE(io != NULL && config != NULL && ret_touch != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid arguments");

    esp_lcd_touch_handle_t tp = calloc(1, sizeof(esp_lcd_touch_t));
    ESP_RETURN_ON_FALSE(tp != NULL, ESP_ERR_NO_MEM, TAG, "alloc touch handle failed");

    gt9895_touch_t *driver = calloc(1, sizeof(gt9895_touch_t));
    if (driver == NULL) {
        free(tp);
        return ESP_ERR_NO_MEM;
    }

    driver->io = io;

    tp->config = *config;
    tp->config.x_max = GT9895_TOUCH_WIDTH;
    tp->config.y_max = GT9895_TOUCH_HEIGHT;
    tp->config.driver_data = driver;
    tp->io = io;
    tp->read_data = gt9895_read_data;
    tp->get_xy = gt9895_get_xy;
    tp->get_track_id = gt9895_get_track_id;
    tp->del = gt9895_del;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    tp->data.lock = lock;

    esp_err_t ret = esp_lcd_touch_gt9895_check_id(driver->io);
    if (ret != ESP_OK) {
        gt9895_del(tp);
        return ret;
    }

    *ret_touch = tp;
    return ESP_OK;
}
