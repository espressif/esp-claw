/*
 * DFRobot FireBeetle 2 ESP32-S3 board-specific factory entries.
 *
 * DFR1092 uses ST7365P (480x320 IPS). There is no dedicated esp_lcd ST7365
 * component in ESP-IDF yet; the panel is brought up via esp_lcd_ili9488 IPS,
 * which matches the 320x480 SPI RGB565 path used by DFRobot GDL.
 */

#include <string.h>
#include "esp_lcd_ili9488.h"
#include "esp_log.h"

static const char *TAG = "DFROBOT_FB2_S3_SETUP";

#define DFR1092_H_RES 480
#define DFR1092_V_RES 320
#define DFR1092_DRAW_BUF_BYTES (DFR1092_H_RES * DFR1092_V_RES * 3)

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};

    if (!io || !panel_dev_config || !ret_panel) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(panel_dev_cfg));

    esp_err_t ret = esp_lcd_new_panel_ili9488_ips(io, &panel_dev_cfg, DFR1092_DRAW_BUF_BYTES, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7365P/ILI9488 IPS panel init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

