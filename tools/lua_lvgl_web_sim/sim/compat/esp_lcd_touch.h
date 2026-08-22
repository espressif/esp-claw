#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef void *esp_lcd_touch_handle_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t strength;
} esp_lcd_touch_point_data_t;

esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t tp,
                                 esp_lcd_touch_point_data_t *point,
                                 uint8_t *point_count,
                                 uint8_t max_point_count);
