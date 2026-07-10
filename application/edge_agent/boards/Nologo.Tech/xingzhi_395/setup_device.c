#include <string.h>
#include "esp_log.h"
#include "dev_display_lcd.h"
#include "esp_lcd_fl7707n.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"

static const char *TAG = "XINGZHI_395";

esp_err_t lcd_dsi_panel_factory_entry_t(esp_lcd_dsi_bus_handle_t dsi_handle, dev_display_lcd_config_t *lcd_cfg, dev_display_lcd_handles_t *lcd_handles)
{

    fl7707n_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = dsi_handle,
            .dpi_config = &lcd_cfg->sub_cfg.dsi.dpi_config,
        },
    };

    const esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = lcd_cfg->sub_cfg.dsi.reset_gpio_num,
        .rgb_ele_order = lcd_cfg->rgb_ele_order,
        .bits_per_pixel = lcd_cfg->bits_per_pixel,
        .vendor_config = &vendor_config,
    };

    esp_err_t ret = esp_lcd_new_panel_fl7707n(lcd_handles->io_handle, &lcd_dev_config, &lcd_handles->panel_handle);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create ek79007 panel: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    return ESP_OK;
}