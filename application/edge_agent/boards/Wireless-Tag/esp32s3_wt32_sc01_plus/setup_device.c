/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file setup_device.c
 * @brief Wireless Tag WT32-SC01 Plus board device initialisation.
 *
 * Display: ST7796UI, 480×320, 8-bit Intel 8080 parallel bus.
 *   ESP32-S3 has SOC_LCD_I80_SUPPORTED but NOT PARLIO, so the native I80
 *   driver (esp_lcd_new_i80_bus / esp_lcd_new_panel_io_i80) is used.
 *
 *   DC/RS : GPIO0    WR  : GPIO47
 *   DB[0] : GPIO9    DB[1]: GPIO46   DB[2]: GPIO3    DB[3]: GPIO8
 *   DB[4] : GPIO18   DB[5]: GPIO17   DB[6]: GPIO16   DB[7]: GPIO15
 *
 * Touch: FT6336U capacitive, ft5x06-compatible, over I2C.
 *   SDA: GPIO6   SCL: GPIO5   INT: GPIO7
 *
 * Shared HW reset on GPIO4 — asserted once before either device is started;
 * both device configs set reset_gpio_num = -1 / need_reset = false.
 */

#include <string.h>
#include "driver/gpio.h"
#include "esp_board_device.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gen_board_device_custom.h"

typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t    panel_handle;
} wt32_lcd_handles_t;

static const char *TAG = "wt32_sc01_plus";

/* ── Pin assignments ─────────────────────────────────────────────────────── */
#define LCD_DC_GPIO   0
#define LCD_WR_GPIO   47
#define LCD_RST_GPIO  4     /* shared with touch */
#define LCD_CS_GPIO   (-1)  /* no CS; bus exclusively owned */

static const gpio_num_t LCD_DATA_GPIOS[8] = {9, 46, 3, 8, 18, 17, 16, 15};

/* ── Display geometry ────────────────────────────────────────────────────── */
#define LCD_WIDTH   480
#define LCD_HEIGHT  320

/* ── I80 bus parameters ──────────────────────────────────────────────────── */
#define LCD_PCLK_HZ        (10 * 1000 * 1000)
#define LCD_MAX_TRANS_BYTES (LCD_WIDTH * LCD_HEIGHT * 2)  /* 16 bpp full frame */
#define LCD_DMA_BURST_SIZE  64

/* ── Shared reset ────────────────────────────────────────────────────────── */
static bool s_reset_done = false;

static void do_shared_reset(void)
{
    if (s_reset_done) {
        return;
    }
    gpio_config_t cfg = {
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = BIT64(LCD_RST_GPIO),
    };
    gpio_config(&cfg);
    gpio_set_level(LCD_RST_GPIO, 0);
    esp_rom_delay_us(20000);
    gpio_set_level(LCD_RST_GPIO, 1);
    esp_rom_delay_us(20000);
    s_reset_done = true;
    ESP_LOGI(TAG, "Shared HW reset on GPIO%d done", LCD_RST_GPIO);
}

/* ── Config exposed to esp_board_manager (custom device) ─────────────────── */
static const dev_custom_display_lcd_config_t s_lcd_config = {
    .name       = "display_lcd",
    .type       = "custom",
    .chip       = "st7796",
    .lcd_width  = LCD_WIDTH,
    .lcd_height = LCD_HEIGHT,
};

static wt32_lcd_handles_t s_lcd_handles;

/* ── Custom display init ─────────────────────────────────────────────────── */
static int display_lcd_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle, ESP_ERR_INVALID_ARG, TAG, "device_handle is NULL");

    do_shared_reset();

    esp_err_t ret;

    /* 1. Create I80 bus */
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num        = LCD_DC_GPIO,
        .wr_gpio_num        = LCD_WR_GPIO,
        .clk_src            = LCD_CLK_SRC_DEFAULT,
        .bus_width          = 8,
        .max_transfer_bytes = LCD_MAX_TRANS_BYTES,
        .dma_burst_size     = LCD_DMA_BURST_SIZE,
    };
    for (int i = 0; i < 8; i++) {
        bus_cfg.data_gpio_nums[i] = LCD_DATA_GPIOS[i];
    }

    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    ret = esp_lcd_new_i80_bus(&bus_cfg, &i80_bus);
    ESP_RETURN_ON_ERROR(ret, TAG, "I80 bus init failed");

    /* 2. Create panel IO */
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num      = LCD_CS_GPIO,
        .pclk_hz          = LCD_PCLK_HZ,
        .trans_queue_depth= 10,
        .lcd_cmd_bits     = 8,
        .lcd_param_bits   = 8,
        .dc_levels = {
            .dc_idle_level  = 0,
            .dc_cmd_level   = 0,
            .dc_dummy_level = 0,
            .dc_data_level  = 1,
        },
        .flags = {
            .cs_active_high  = false,
            .swap_color_bytes = false,
        },
    };

    esp_lcd_panel_io_handle_t io_handle = NULL;
    ret = esp_lcd_new_panel_io_i80(i80_bus, &io_cfg, &io_handle);
    ESP_GOTO_ON_ERROR(ret, err_del_bus, TAG, "panel IO init failed");

    /* 3. Create ST7796 panel */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num  = -1,          /* reset already done via do_shared_reset() */
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian     = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel  = 16,
        .flags = { .reset_active_high = false },
        .vendor_config   = NULL,
    };

    esp_lcd_panel_handle_t panel_handle = NULL;
    ret = esp_lcd_new_panel_st7796(io_handle, &panel_cfg, &panel_handle);
    ESP_GOTO_ON_ERROR(ret, err_del_io, TAG, "ST7796 panel init failed");

    /* 4. Init panel and apply orientation */
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel_handle),
                      err_del_panel, TAG, "panel init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_invert_color(panel_handle, true),
                      err_del_panel, TAG, "invert color failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_swap_xy(panel_handle, true),
                      err_del_panel, TAG, "swap XY failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(panel_handle, true),
                      err_del_panel, TAG, "display on failed");

    /* 5. Register with esp_board_manager */
    s_lcd_handles.io_handle    = io_handle;
    s_lcd_handles.panel_handle = panel_handle;
    esp_board_device_override_config("display_lcd", &s_lcd_config, sizeof(s_lcd_config));
    *device_handle = &s_lcd_handles;

    ESP_LOGI(TAG, "ST7796 ready (%dx%d @ %lu MHz)",
             LCD_WIDTH, LCD_HEIGHT, (unsigned long)(LCD_PCLK_HZ / 1000000));
    return ESP_OK;

err_del_panel:
    esp_lcd_panel_del(panel_handle);
err_del_io:
    esp_lcd_panel_io_del(io_handle);
err_del_bus:
    esp_lcd_del_i80_bus(i80_bus);
    return ret;
}

static int display_lcd_deinit(void *device_handle)
{
    wt32_lcd_handles_t *handles = (wt32_lcd_handles_t *)device_handle;
    if (handles) {
        if (handles->panel_handle) {
            esp_lcd_panel_del(handles->panel_handle);
            handles->panel_handle = NULL;
        }
        if (handles->io_handle) {
            esp_lcd_panel_io_del(handles->io_handle);
            handles->io_handle = NULL;
        }
    }
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(display_lcd, display_lcd_init, display_lcd_deinit);

/* ── Touch factory (used by lcd_touch_i2c device type) ──────────────────── */
esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    do_shared_reset();
    esp_err_t ret = esp_lcd_touch_new_i2c_ft5x06(io, touch_dev_config, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ft5x06 init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
