#include "dev_display_lcd.h"
#include "esp_err.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_lcd_nv3051f.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"

static const char *TAG = "METALIO_CLAW_4_SETUP_DEVICE";

esp_err_t io_expander_factory_entry_t(i2c_master_bus_handle_t i2c_handle,
                                      const uint16_t dev_addr,
                                      esp_io_expander_handle_t *handle_ret)
{
    esp_err_t ret = esp_io_expander_new_i2c_tca95xx_16bit(i2c_handle, dev_addr, handle_ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create TCA9555 IO expander: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t lcd_dsi_panel_factory_entry_t(esp_lcd_dsi_bus_handle_t dsi_handle,
                                        dev_display_lcd_config_t *lcd_cfg,
                                        dev_display_lcd_handles_t *lcd_handles)
{
    nv3051f_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = dsi_handle,
            .dpi_config = &lcd_cfg->sub_cfg.dsi.dpi_config,
        },
    };

    const esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = lcd_cfg->sub_cfg.dsi.reset_gpio_num,
        .rgb_ele_order = lcd_cfg->rgb_ele_order,
        .bits_per_pixel = lcd_cfg->bits_per_pixel,
        .data_endian = lcd_cfg->data_endian,
        .flags = {
            .reset_active_high = lcd_cfg->sub_cfg.dsi.reset_active_high,
        },
        .vendor_config = &vendor_config,
    };

    esp_err_t ret = esp_lcd_new_panel_nv3051f(lcd_handles->io_handle,
                                              &lcd_dev_config,
                                              &lcd_handles->panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create NV3051F panel: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t ret = esp_lcd_touch_new_i2c_gt911(io, touch_dev_config, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create GT911 touch: %s", esp_err_to_name(ret));
    }
    return ret;
}
