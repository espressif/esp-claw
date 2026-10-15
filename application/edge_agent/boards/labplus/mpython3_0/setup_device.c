/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-specific setup for the Labplus mPython 3.0 / 掌控板 3.0.
 *
 * The onboard ST7789 panel has no dedicated reset line and uses the stock esp_lcd
 * ST7789 driver, so the SPI display path only needs the panel factory hook that
 * dev_display_lcd_sub_spi.c resolves at link time.
 *
 * Two panel quirks are handled here.
 *
 * 1. Window offset
 *    The 172-pixel-wide glass is centred inside the ST7789's 240x320 frame memory,
 *    leaving a 34-pixel margin on each side. Without compensating for it the driver
 *    writes into the margin and the far edge of the glass is never addressed, which
 *    shows up as a band of uninitialised pixels along one edge. The board manager's
 *    display_lcd config has no gap field, so it is applied here. With swap_xy
 *    enabled the driver works in 320x172 landscape coordinates, where logical Y
 *    runs along the glass' short axis, so the offset belongs on Y.
 *
 * 2. Orientation
 *    board_devices.yaml sets both mirror_x and mirror_y, which the board manager
 *    turns into MADCTL = MX|MY|MV. That combination was determined on real
 *    hardware: with MV alone this panel renders the frame rotated 180 degrees, so
 *    both mirror bits have to be set. The byte is written explicitly here as well
 *    so the result does not depend on how the board manager's mirror and swap
 *    helpers combine.
 */

#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

static const char *TAG = "MPYTHON3_0_SETUP_DEVICE";

/*!< Centring offset of the 172-pixel-wide glass in the ST7789 240-pixel frame memory. */
#define MPYTHON3_0_LCD_GAP_PX  ((240 - 172) / 2)

/*!< MADCTL that renders this panel upright: MX|MY|MV, matching board_devices.yaml. */
#define MPYTHON3_0_LCD_MADCTL  (0xE0)

/*!< ST7789 command used directly: esp_lcd keeps MADCTL private. */
#define ST7789_CMD_MADCTL       (0x36)

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io != NULL && panel_dev_config != NULL && ret_panel != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ST7789 panel arguments");

    esp_err_t ret = esp_lcd_new_panel_st7789(io, panel_dev_config, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_set_gap(*ret_panel, 0, MPYTHON3_0_LCD_GAP_PX);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set ST7789 gap: %s", esp_err_to_name(ret));
        return ret;
    }

    const uint8_t madctl = MPYTHON3_0_LCD_MADCTL;
    ret = esp_lcd_panel_io_tx_param(io, ST7789_CMD_MADCTL, &madctl, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write ST7789 MADCTL: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "ST7789 ready: MADCTL=0x%02X, y_gap=%d px",
             MPYTHON3_0_LCD_MADCTL, MPYTHON3_0_LCD_GAP_PX);
    return ESP_OK;
}
