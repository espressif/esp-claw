/* LilyGO T-Display-S3 AMOLED V1.1: RM67162 panel factory.
 * SPI bus and panel IO are created by ESP Board Manager.
 * GPIO38 intentionally untouched: V1.x LED, not assumed display power enable.
 */
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_rm67162.h"

esp_err_t lcd_panel_factory_entry_t(
    esp_lcd_panel_io_handle_t io,
    const esp_lcd_panel_dev_config_t *panel_config,
    esp_lcd_panel_handle_t *panel_handle)
{
    return esp_lcd_new_panel_rm67162(io, panel_config, panel_handle);
}
