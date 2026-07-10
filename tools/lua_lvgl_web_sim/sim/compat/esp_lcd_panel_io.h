#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void *esp_lcd_panel_io_handle_t;

typedef struct {
    int dummy;
} esp_lcd_panel_io_event_data_t;

typedef bool (*esp_lcd_panel_io_color_done_cb_t)(esp_lcd_panel_io_handle_t panel_io,
                                                 esp_lcd_panel_io_event_data_t *edata,
                                                 void *user_ctx);

typedef struct {
    esp_lcd_panel_io_color_done_cb_t on_color_trans_done;
} esp_lcd_panel_io_callbacks_t;

esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io,
                                                    const esp_lcd_panel_io_callbacks_t *callbacks,
                                                    void *user_ctx);
