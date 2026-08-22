#pragma once

#include "esp_err.h"

typedef void *esp_lcd_panel_handle_t;

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel,
                                    int x_start,
                                    int y_start,
                                    int x_end,
                                    int y_end,
                                    const void *color_data);
