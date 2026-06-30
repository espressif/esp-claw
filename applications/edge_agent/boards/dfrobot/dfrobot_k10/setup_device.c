/*
 * DFRobot K10 board-specific factory entries for ESP Board Manager.
 */

#include "esp_log.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_lcd_ili9341.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DFROBOT_K10_SETUP_DEVICE";

esp_err_t io_expander_factory_entry_t(i2c_master_bus_handle_t i2c_handle, const uint16_t dev_addr, esp_io_expander_handle_t *handle_ret)
{
    esp_err_t ret = esp_io_expander_new_i2c_tca95xx_16bit(i2c_handle, dev_addr, handle_ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create TCA95xx IO expander handle: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Match original df-k10 power-up/reset sequence:
     * drive P0/P1 low briefly, then high, then leave inputs for keys handled by board manager.
     */
    const uint32_t reset_mask = IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1;
    ret = esp_io_expander_set_dir(*handle_ret, reset_mask, IO_EXPANDER_OUTPUT);
    if (ret == ESP_OK) {
        ret = esp_io_expander_set_level(*handle_ret, reset_mask, 0);
    }
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        ret = esp_io_expander_set_level(*handle_ret, reset_mask, 1);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "IO expander reset sequence failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = esp_lcd_new_panel_ili9341(io, panel_dev_config, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ILI9341 panel handle: %s", esp_err_to_name(ret));
    }
    return ret;
}
