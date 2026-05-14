/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <string.h>

#include "esp_board_device.h"
#include "esp_board_periph.h"
#include "esp_io_expander.h"
#include "esp_io_expander_pi4ioe5v6408.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st7123.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_log.h"
#include "dev_display_lcd.h"
#include "disp_init_data.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "M5STACK_TAB5_SETUP_DEVICE";

typedef enum {
    TAB5_VARIANT_UNKNOWN = 0,
    TAB5_VARIANT_ILI9881C_GT911,
    TAB5_VARIANT_ST7123,
} tab5_variant_t;

static tab5_variant_t s_tab5_variant = TAB5_VARIANT_UNKNOWN;
static uint16_t s_tab5_touch_addr    = 0;

#define TAB5_TOUCH_INT_GPIO        GPIO_NUM_23
#define TAB5_IO1_OUTPUT_MASK       ((1U << 0) | (1U << 1) | (1U << 2) | (1U << 3) | (1U << 4) | (1U << 5) | (1U << 6))
#define TAB5_IO1_INPUT_MASK        (1U << 7)
#define TAB5_IO1_PULLUP_MASK       TAB5_IO1_OUTPUT_MASK
#define TAB5_IO1_RESET_LOW_OUTPUT  0x46U
#define TAB5_IO1_RESET_HIGH_OUTPUT 0x76U
#define TAB5_IO2_OUTPUT_MASK       ((1U << 0) | (1U << 3) | (1U << 4) | (1U << 5) | (1U << 7))
#define TAB5_IO2_INPUT_MASK        ((1U << 1) | (1U << 2) | (1U << 6))
#define TAB5_IO2_PULLUP_MASK       ((1U << 0) | (1U << 3) | (1U << 4) | (1U << 5) | (1U << 7))
#define TAB5_IO2_PULLDOWN_MASK     (1U << 6)
#define TAB5_IO2_OUTPUT_VALUE      0x89U
#define TAB5_PROBE_RETRY_COUNT     3

static const char *variant_name(tab5_variant_t variant)
{
    switch (variant) {
        case TAB5_VARIANT_ILI9881C_GT911:
            return "ILI9881C + GT911";
        case TAB5_VARIANT_ST7123:
            return "ST7123 + ST7123";
        default:
            return "unknown";
    }
}

static esp_err_t tab5_set_expander_output_levels(esp_io_expander_handle_t handle, uint32_t output_mask,
                                                 uint8_t output_value)
{
    uint32_t high_mask = output_mask & output_value;
    uint32_t low_mask  = output_mask & (~output_value & 0xFFU);

    if (low_mask) {
        esp_err_t ret = esp_io_expander_set_level(handle, low_mask, 0);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (high_mask) {
        esp_err_t ret = esp_io_expander_set_level(handle, high_mask, 1);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t tab5_configure_expander(esp_io_expander_handle_t handle, uint32_t output_mask, uint32_t input_mask,
                                         uint8_t output_value, uint32_t pullup_mask, uint32_t pulldown_mask)
{
    esp_err_t ret = ESP_OK;

    if (output_mask) {
        ret = esp_io_expander_set_dir(handle, output_mask, IO_EXPANDER_OUTPUT);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = esp_io_expander_set_output_mode(handle, output_mask, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (input_mask) {
        ret = esp_io_expander_set_dir(handle, input_mask, IO_EXPANDER_INPUT);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (pullup_mask) {
        ret = esp_io_expander_set_pullupdown(handle, pullup_mask, IO_EXPANDER_PULL_UP);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (pulldown_mask) {
        ret = esp_io_expander_set_pullupdown(handle, pulldown_mask, IO_EXPANDER_PULL_DOWN);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return tab5_set_expander_output_levels(handle, output_mask, output_value);
}

static esp_err_t tab5_get_io_expander(const char *name, esp_io_expander_handle_t **out_handle)
{
    esp_err_t ret = esp_board_device_get_handle(name, (void **)out_handle);
    if (ret == ESP_OK && out_handle && *out_handle) {
        return ESP_OK;
    }

    ret = esp_board_device_init(name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init %s: %s", name, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_board_device_get_handle(name, (void **)out_handle);
    if (ret != ESP_OK || out_handle == NULL || *out_handle == NULL) {
        ESP_LOGE(TAG, "Failed to get %s handle: %s", name, esp_err_to_name(ret));
        return ret != ESP_OK ? ret : ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t tab5_prepare_touch_addr_select(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << TAB5_TOUCH_INT_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    return gpio_set_level(TAB5_TOUCH_INT_GPIO, 1);
}

static esp_err_t tab5_apply_reset_sequence(void)
{
    esp_io_expander_handle_t *io1 = NULL;
    esp_io_expander_handle_t *io2 = NULL;
    esp_err_t ret                 = tab5_get_io_expander("gpio_expander", &io1);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = tab5_get_io_expander("gpio_expander_2", &io2);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tab5_prepare_touch_addr_select();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to drive TP_INT high: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = tab5_configure_expander(*io1, TAB5_IO1_OUTPUT_MASK, TAB5_IO1_INPUT_MASK, TAB5_IO1_RESET_LOW_OUTPUT,
                                  TAB5_IO1_PULLUP_MASK, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure gpio_expander: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = tab5_configure_expander(*io2, TAB5_IO2_OUTPUT_MASK, TAB5_IO2_INPUT_MASK, TAB5_IO2_OUTPUT_VALUE,
                                  TAB5_IO2_PULLUP_MASK, TAB5_IO2_PULLDOWN_MASK);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure gpio_expander_2: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    ret = tab5_set_expander_output_levels(*io1, TAB5_IO1_OUTPUT_MASK, TAB5_IO1_RESET_HIGH_OUTPUT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to release Tab5 LCD/touch reset: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static esp_err_t tab5_probe_variant_with_io(esp_lcd_panel_io_handle_t io, tab5_variant_t *out_variant)
{
    static const uint8_t ili_page1[] = {0x98, 0x81, 0x01};
    static const uint8_t ili_page0[] = {0x98, 0x81, 0x00};

    vTaskDelay(pdMS_TO_TICKS(80));

    for (int attempt = 0; attempt < TAB5_PROBE_RETRY_COUNT; ++attempt) {
        uint8_t st_id[2] = {0};
        esp_err_t ret    = esp_lcd_panel_io_rx_param(io, 0xF4, st_id, sizeof(st_id));
        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "ST7123 ID: %02X %02X", st_id[0], st_id[1]);
            if (st_id[0] == 0x71 && st_id[1] == 0x23) {
                *out_variant = TAB5_VARIANT_ST7123;
                return ESP_OK;
            }
        } else {
            ESP_LOGW(TAG, "ST7123 ID read failed on attempt %d: %s", attempt + 1, esp_err_to_name(ret));
        }

        uint8_t ili_id[3] = {0};
        ret               = esp_lcd_panel_io_tx_param(io, 0xFF, ili_page1, sizeof(ili_page1));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ILI9881C page select failed on attempt %d: %s", attempt + 1, esp_err_to_name(ret));
            continue;
        }

        esp_err_t read_ret = ESP_OK;
        if (read_ret == ESP_OK) {
            read_ret = esp_lcd_panel_io_rx_param(io, 0x00, &ili_id[0], 1);
        }
        if (read_ret == ESP_OK) {
            read_ret = esp_lcd_panel_io_rx_param(io, 0x01, &ili_id[1], 1);
        }
        if (read_ret == ESP_OK) {
            read_ret = esp_lcd_panel_io_rx_param(io, 0x02, &ili_id[2], 1);
        }

        esp_err_t restore_ret = esp_lcd_panel_io_tx_param(io, 0xFF, ili_page0, sizeof(ili_page0));
        if (restore_ret != ESP_OK) {
            ESP_LOGW(TAG, "ILI9881C page restore failed on attempt %d: %s", attempt + 1, esp_err_to_name(restore_ret));
        }

        if (read_ret == ESP_OK) {
            ESP_LOGD(TAG, "ILI9881C ID: %02X %02X %02X", ili_id[0], ili_id[1], ili_id[2]);
            if (ili_id[0] == 0x98 && ili_id[1] == 0x81) {
                *out_variant = TAB5_VARIANT_ILI9881C_GT911;
                return ESP_OK;
            }
        } else {
            ESP_LOGW(TAG, "ILI9881C ID read failed on attempt %d: %s", attempt + 1, esp_err_to_name(read_ret));
        }
    }

    *out_variant = TAB5_VARIANT_UNKNOWN;
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t tab5_create_probe_io(esp_lcd_panel_io_handle_t *out_io, const char **out_dsi_name)
{
    dev_display_lcd_config_t *lcd_cfg   = NULL;
    esp_lcd_dsi_bus_handle_t dsi_handle = NULL;
    esp_err_t ret                       = esp_board_device_get_config("display_lcd", (void **)&lcd_cfg);
    if (ret != ESP_OK || lcd_cfg == NULL) {
        ESP_LOGE(TAG, "Failed to get display_lcd config: %s", esp_err_to_name(ret));
        return ret != ESP_OK ? ret : ESP_FAIL;
    }

    ret = esp_board_periph_ref_handle(lcd_cfg->sub_cfg.dsi.dsi_name, (void **)&dsi_handle);
    if (ret != ESP_OK || dsi_handle == NULL) {
        ESP_LOGE(TAG, "Failed to get DSI handle for probe: %s", esp_err_to_name(ret));
        return ret != ESP_OK ? ret : ESP_FAIL;
    }

    ret = esp_lcd_new_panel_io_dbi(dsi_handle, &lcd_cfg->sub_cfg.dsi.dbi_config, out_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create temporary DSI IO: %s", esp_err_to_name(ret));
        esp_board_periph_unref_handle(lcd_cfg->sub_cfg.dsi.dsi_name);
        return ret;
    }

    *out_dsi_name = lcd_cfg->sub_cfg.dsi.dsi_name;
    return ESP_OK;
}

static esp_err_t tab5_detect_variant_once(esp_lcd_panel_io_handle_t existing_io)
{
    if (s_tab5_variant != TAB5_VARIANT_UNKNOWN) {
        return ESP_OK;
    }

    esp_err_t ret = tab5_apply_reset_sequence();
    if (ret != ESP_OK) {
        return ret;
    }

    esp_lcd_panel_io_handle_t probe_io = existing_io;
    const char *probe_dsi_name         = NULL;

    if (probe_io == NULL) {
        ret = tab5_create_probe_io(&probe_io, &probe_dsi_name);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    tab5_variant_t detected = TAB5_VARIANT_UNKNOWN;
    ret                     = tab5_probe_variant_with_io(probe_io, &detected);

    if (existing_io == NULL && probe_io != NULL) {
        esp_lcd_panel_io_del(probe_io);
        if (probe_dsi_name) {
            esp_board_periph_unref_handle(probe_dsi_name);
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to detect Tab5 panel variant: %s", esp_err_to_name(ret));
        return ret;
    }

    s_tab5_variant    = detected;
    s_tab5_touch_addr = (detected == TAB5_VARIANT_ST7123) ? 0x55 : 0x14;
    ESP_LOGI(TAG, "Detected Tab5 variant: %s", variant_name(s_tab5_variant));
    return ESP_OK;
}

static void tab5_apply_ili_timing(dev_display_lcd_config_t *lcd_cfg)
{
    lcd_cfg->sub_cfg.dsi.dpi_config.dpi_clock_freq_mhz             = 80;
    lcd_cfg->sub_cfg.dsi.dpi_config.video_timing.hsync_back_porch  = 140;
    lcd_cfg->sub_cfg.dsi.dpi_config.video_timing.hsync_front_porch = 40;
    lcd_cfg->sub_cfg.dsi.dpi_config.video_timing.vsync_back_porch  = 20;
    lcd_cfg->sub_cfg.dsi.dpi_config.video_timing.vsync_pulse_width = 4;
    lcd_cfg->sub_cfg.dsi.dpi_config.video_timing.vsync_front_porch = 20;
}

static void tab5_apply_st_timing(dev_display_lcd_config_t *lcd_cfg)
{
    lcd_cfg->data_endian                                           = LCD_RGB_DATA_ENDIAN_LITTLE;
    lcd_cfg->sub_cfg.dsi.dpi_config.dpi_clock_freq_mhz             = 80;
    lcd_cfg->sub_cfg.dsi.dpi_config.num_fbs                        = 2;
    lcd_cfg->sub_cfg.dsi.dpi_config.video_timing.hsync_back_porch  = 40;
    lcd_cfg->sub_cfg.dsi.dpi_config.video_timing.hsync_pulse_width = 2;
    lcd_cfg->sub_cfg.dsi.dpi_config.video_timing.hsync_front_porch = 40;
    lcd_cfg->sub_cfg.dsi.dpi_config.video_timing.vsync_back_porch  = 8;
    lcd_cfg->sub_cfg.dsi.dpi_config.video_timing.vsync_pulse_width = 2;
    lcd_cfg->sub_cfg.dsi.dpi_config.video_timing.vsync_front_porch = 220;
}

esp_err_t io_expander_factory_entry_t(i2c_master_bus_handle_t i2c_handle, const uint16_t dev_addr,
                                      esp_io_expander_handle_t *handle_ret)
{
    esp_err_t ret = esp_io_expander_new_i2c_pi4ioe5v6408(i2c_handle, dev_addr, handle_ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create IO expander handle");
        return ret;
    }
    return ESP_OK;
}

esp_err_t lcd_dsi_panel_factory_entry_t(esp_lcd_dsi_bus_handle_t dsi_handle, dev_display_lcd_config_t *lcd_cfg,
                                        dev_display_lcd_handles_t *lcd_handles)
{
    dev_display_lcd_config_t lcd_cfg_local = *lcd_cfg;
    dev_display_lcd_config_t *runtime_cfg  = &lcd_cfg_local;
    esp_err_t ret                          = tab5_detect_variant_once(lcd_handles->io_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_tab5_variant == TAB5_VARIANT_ILI9881C_GT911) {
        tab5_apply_ili_timing(runtime_cfg);

        ili9881c_vendor_config_t vendor_config = {
            .init_cmds      = tab5_ili9881c_init_cmds,
            .init_cmds_size = sizeof(tab5_ili9881c_init_cmds) / sizeof(tab5_ili9881c_init_cmds[0]),
            .mipi_config =
                {
                    .dsi_bus    = dsi_handle,
                    .dpi_config = &runtime_cfg->sub_cfg.dsi.dpi_config,
                },
        };

        esp_lcd_panel_dev_config_t lcd_dev_config = {
            .reset_gpio_num = runtime_cfg->sub_cfg.dsi.reset_gpio_num,
            .rgb_ele_order  = runtime_cfg->rgb_ele_order,
            .bits_per_pixel = runtime_cfg->bits_per_pixel,
            .data_endian    = runtime_cfg->data_endian,
            .flags =
                {
                    .reset_active_high = runtime_cfg->sub_cfg.dsi.reset_active_high,
                },
            .vendor_config = &vendor_config,
        };

        ret = esp_lcd_new_panel_ili9881c(lcd_handles->io_handle, &lcd_dev_config, &lcd_handles->panel_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create ili9881c panel: %s", esp_err_to_name(ret));
            return ret;
        }
        return ESP_OK;
    }

    if (s_tab5_variant == TAB5_VARIANT_ST7123) {
        tab5_apply_st_timing(runtime_cfg);

        st7123_vendor_config_t vendor_config = {
            .init_cmds      = tab5_st7123_init_cmds,
            .init_cmds_size = sizeof(tab5_st7123_init_cmds) / sizeof(tab5_st7123_init_cmds[0]),
            .mipi_config =
                {
                    .dsi_bus    = dsi_handle,
                    .dpi_config = &runtime_cfg->sub_cfg.dsi.dpi_config,
                },
        };

        esp_lcd_panel_dev_config_t lcd_dev_config = {
            .reset_gpio_num = runtime_cfg->sub_cfg.dsi.reset_gpio_num,
            .rgb_ele_order  = runtime_cfg->rgb_ele_order,
            .bits_per_pixel = runtime_cfg->bits_per_pixel,
            .data_endian    = runtime_cfg->data_endian,
            .flags =
                {
                    .reset_active_high = runtime_cfg->sub_cfg.dsi.reset_active_high,
                },
            .vendor_config = &vendor_config,
        };

        ret = esp_lcd_new_panel_st7123(lcd_handles->io_handle, &lcd_dev_config, &lcd_handles->panel_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create st7123 panel: %s", esp_err_to_name(ret));
            return ret;
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Unknown Tab5 panel variant");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t ret = tab5_detect_variant_once(NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_tab5_variant == TAB5_VARIANT_ST7123) {
        ret = esp_lcd_touch_new_i2c_st7123(io, touch_dev_config, ret_touch);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create st7123 touch driver: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    ret = esp_lcd_touch_new_i2c_gt911(io, touch_dev_config, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create gt911 touch driver: %s", esp_err_to_name(ret));
    }
    return ret;
}
