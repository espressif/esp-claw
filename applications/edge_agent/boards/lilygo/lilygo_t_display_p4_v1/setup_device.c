/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdlib.h>

#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"
#include "driver/i2c_master.h"
#include "esp_board_periph.h"
#include "esp_board_device.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_lcd_hi8561.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_rm69a10.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt9895.h"
#include "esp_lcd_touch_hi8561.h"
#include "esp_log.h"

static const char *TAG = "lilygo_t_display_p4";

#define LILYGO_TOUCH_DEVICE_NAME        "lcd_touch"
#define LILYGO_TOUCH_I2C_NAME           "i2c_port0"
#define LILYGO_TOUCH_ADDR_GT9895        0xbau  /* 8-bit / left-shifted */
#define LILYGO_TOUCH_ADDR_GT9895_ALT    0x28u  /* 8-bit / left-shifted */
#define LILYGO_TOUCH_ADDR_HI8561        0xd0u  /* 8-bit / left-shifted */
#define LILYGO_TOUCH_I2C_FREQ_HZ        400000u
#define LILYGO_TOUCH_I2C_TIMEOUT_MS     200
#define ARRAY_SIZE(a)                   (sizeof(a) / sizeof((a)[0]))

static const uint16_t s_gt9895_touch_addrs[] = {
    LILYGO_TOUCH_ADDR_GT9895,
    LILYGO_TOUCH_ADDR_GT9895_ALT,
};

static const char *touch_i2c_name_or_default(const dev_lcd_touch_config_t *touch_cfg)
{
    const char *i2c_name = touch_cfg->sub_cfg.i2c.i2c_name;
    return (i2c_name != NULL && i2c_name[0] != '\0') ? i2c_name : LILYGO_TOUCH_I2C_NAME;
}

static uint32_t touch_i2c_freq_or_default(const dev_lcd_touch_config_t *touch_cfg)
{
    uint32_t scl_speed_hz = touch_cfg->sub_cfg.i2c.io_i2c_config.scl_speed_hz;
    return scl_speed_hz != 0 ? scl_speed_hz : LILYGO_TOUCH_I2C_FREQ_HZ;
}

static esp_err_t touch_i2c_ref(const char *i2c_name, void **ret_i2c_bus)
{
    esp_err_t ret = esp_board_periph_ref_handle(i2c_name, ret_i2c_bus);
    ESP_RETURN_ON_FALSE(ret == ESP_OK && *ret_i2c_bus != NULL, ret == ESP_OK ? ESP_FAIL : ret,
                        TAG, "failed to get %s for touch", i2c_name);
    return ESP_OK;
}

static void fill_gt9895_io_config(esp_lcd_panel_io_i2c_config_t *io_config,
                                  uint16_t touch_addr)
{
    io_config->dev_addr = touch_addr >> 1;
    io_config->control_phase_bytes = 1;
    io_config->dc_bit_offset = 0;
    io_config->lcd_cmd_bits = 8;
    io_config->lcd_param_bits = 0;
    if (io_config->scl_speed_hz == 0) {
        io_config->scl_speed_hz = LILYGO_TOUCH_I2C_FREQ_HZ;
    }
    io_config->flags.disable_control_phase = true;
}

static esp_err_t new_i2c_panel_io(i2c_master_bus_handle_t i2c_bus,
                                  const esp_lcd_panel_io_i2c_config_t *io_config,
                                  esp_lcd_panel_io_handle_t *ret_io)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    return esp_lcd_new_panel_io_i2c(i2c_bus, io_config, ret_io);
#else
    return esp_lcd_new_panel_io_i2c_v2(i2c_bus, io_config, ret_io);
#endif
}

static bool s_panel_variant_detected;
static bool s_panel_is_rm69a10;

typedef struct {
    /* Must stay first: Board Manager stores this member address as the device handle. */
    dev_lcd_touch_handles_t handles;
    const char *i2c_name;
    i2c_master_dev_handle_t i2c_dev_handle;
    bool owns_io;
    bool owns_i2c_dev;
    bool owns_i2c_ref;
} lilygo_lcd_touch_handles_t;

typedef struct {
    const char *name;
    uint16_t width;
    uint16_t height;
    uint32_t dpi_clock_freq_mhz;
    uint16_t hsync_pulse_width;
    uint16_t hsync_back_porch;
    uint16_t hsync_front_porch;
    uint16_t vsync_pulse_width;
    uint16_t vsync_back_porch;
    uint16_t vsync_front_porch;
} lilygo_panel_timing_t;

static const lilygo_panel_timing_t s_timing_hi8561 = {
    .name = "hi8561",
    .width = 540,
    .height = 1168,
    .dpi_clock_freq_mhz = 60,
    .hsync_pulse_width = 28,
    .hsync_back_porch = 26,
    .hsync_front_porch = 20,
    .vsync_pulse_width = 2,
    .vsync_back_porch = 22,
    .vsync_front_porch = 200,
};

static const lilygo_panel_timing_t s_timing_rm69a10 = {
    .name = "rm69a10",
    .width = 568,
    .height = 1232,
    .dpi_clock_freq_mhz = 60,
    .hsync_pulse_width = 50,
    .hsync_back_porch = 150,
    .hsync_front_porch = 50,
    .vsync_pulse_width = 40,
    .vsync_back_porch = 120,
    .vsync_front_porch = 80,
};

static void apply_timing(dev_display_lcd_config_t *cfg, const lilygo_panel_timing_t *timing)
{
    cfg->chip = timing->name;
    cfg->lcd_width = timing->width;
    cfg->lcd_height = timing->height;
    cfg->sub_cfg.dsi.dpi_config.dpi_clock_freq_mhz = timing->dpi_clock_freq_mhz;
    cfg->sub_cfg.dsi.dpi_config.video_timing.h_size = timing->width;
    cfg->sub_cfg.dsi.dpi_config.video_timing.v_size = timing->height;
    cfg->sub_cfg.dsi.dpi_config.video_timing.hsync_pulse_width = timing->hsync_pulse_width;
    cfg->sub_cfg.dsi.dpi_config.video_timing.hsync_back_porch = timing->hsync_back_porch;
    cfg->sub_cfg.dsi.dpi_config.video_timing.hsync_front_porch = timing->hsync_front_porch;
    cfg->sub_cfg.dsi.dpi_config.video_timing.vsync_pulse_width = timing->vsync_pulse_width;
    cfg->sub_cfg.dsi.dpi_config.video_timing.vsync_back_porch = timing->vsync_back_porch;
    cfg->sub_cfg.dsi.dpi_config.video_timing.vsync_front_porch = timing->vsync_front_porch;
}

static bool probe_gt9895_touch_id(uint16_t touch_addr)
{
    void *i2c_bus_handle = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    bool found = false;

    esp_err_t ret = touch_i2c_ref(LILYGO_TOUCH_I2C_NAME, &i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get %s for GT9895 probe: %s", LILYGO_TOUCH_I2C_NAME,
                 esp_err_to_name(ret));
        return false;
    }

    esp_lcd_panel_io_i2c_config_t io_i2c_config = {0};
    fill_gt9895_io_config(&io_i2c_config, touch_addr);

    ret = i2c_master_probe((i2c_master_bus_handle_t)i2c_bus_handle,
                           io_i2c_config.dev_addr, LILYGO_TOUCH_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        esp_board_periph_unref_handle(LILYGO_TOUCH_I2C_NAME);
        return false;
    }

    ret = new_i2c_panel_io((i2c_master_bus_handle_t)i2c_bus_handle, &io_i2c_config, &io);
    if (ret == ESP_OK) {
        found = (esp_lcd_touch_gt9895_check_id(io) == ESP_OK);
        esp_lcd_panel_io_del(io);
    }

    esp_board_periph_unref_handle(LILYGO_TOUCH_I2C_NAME);
    return found;
}

static bool detect_rm69a10_panel(void)
{
    if (!s_panel_variant_detected) {
        for (size_t i = 0; i < ARRAY_SIZE(s_gt9895_touch_addrs); i++) {
            if (probe_gt9895_touch_id(s_gt9895_touch_addrs[i])) {
                s_panel_is_rm69a10 = true;
                break;
            }
        }
        s_panel_variant_detected = true;
    }
    return s_panel_is_rm69a10;
}

static esp_err_t create_gt9895_touch_io(const dev_lcd_touch_config_t *touch_cfg,
                                        esp_lcd_panel_io_handle_t *ret_io,
                                        const char **ret_i2c_name)
{
    ESP_RETURN_ON_FALSE(touch_cfg != NULL && ret_io != NULL && ret_i2c_name != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid GT9895 IO args");

    void *i2c_bus_handle = NULL;
    const char *i2c_name = touch_i2c_name_or_default(touch_cfg);

    esp_err_t ret = touch_i2c_ref(i2c_name, &i2c_bus_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to get %s for GT9895 touch", i2c_name);

    for (size_t i = 0; i < ARRAY_SIZE(s_gt9895_touch_addrs); i++) {
        uint16_t touch_addr = s_gt9895_touch_addrs[i];
        esp_lcd_panel_io_i2c_config_t io_i2c_config = touch_cfg->sub_cfg.i2c.io_i2c_config;
        fill_gt9895_io_config(&io_i2c_config, touch_addr);

        ret = i2c_master_probe((i2c_master_bus_handle_t)i2c_bus_handle,
                               io_i2c_config.dev_addr, LILYGO_TOUCH_I2C_TIMEOUT_MS);
        if (ret != ESP_OK) {
            continue;
        }

        esp_lcd_panel_io_handle_t io = NULL;
        ret = new_i2c_panel_io((i2c_master_bus_handle_t)i2c_bus_handle, &io_i2c_config, &io);
        if (ret != ESP_OK) {
            continue;
        }

        ret = esp_lcd_touch_gt9895_check_id(io);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "GT9895 touch found at addr: 0x%02x", touch_addr);
            *ret_io = io;
            *ret_i2c_name = i2c_name;
            return ESP_OK;
        }
        esp_lcd_panel_io_del(io);
    }

    esp_board_periph_unref_handle(i2c_name);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t create_hi8561_touch_dev(const dev_lcd_touch_config_t *touch_cfg,
                                         i2c_master_dev_handle_t *ret_dev,
                                         const char **ret_i2c_name)
{
    ESP_RETURN_ON_FALSE(touch_cfg != NULL && ret_dev != NULL && ret_i2c_name != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid HI8561 touch args");

    void *i2c_bus_handle = NULL;
    const char *i2c_name = touch_i2c_name_or_default(touch_cfg);

    esp_err_t ret = touch_i2c_ref(i2c_name, &i2c_bus_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to get %s for HI8561 touch", i2c_name);

    uint32_t scl_speed_hz = touch_i2c_freq_or_default(touch_cfg);
    const uint16_t dev_addr = LILYGO_TOUCH_ADDR_HI8561 >> 1;

    ret = i2c_master_probe((i2c_master_bus_handle_t)i2c_bus_handle,
                           dev_addr, LILYGO_TOUCH_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        esp_board_periph_unref_handle(i2c_name);
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = scl_speed_hz,
    };

    ret = i2c_master_bus_add_device((i2c_master_bus_handle_t)i2c_bus_handle,
                                    &dev_cfg, ret_dev);
    if (ret != ESP_OK) {
        esp_board_periph_unref_handle(i2c_name);
        return ret;
    }

    *ret_i2c_name = i2c_name;
    ESP_LOGI(TAG, "HI8561 touch found at addr: 0x%02x", dev_addr);
    return ESP_OK;
}

static void cleanup_lcd_touch(lilygo_lcd_touch_handles_t *touch)
{
    if (touch == NULL) {
        return;
    }
    if (touch->handles.touch_handle != NULL) {
        esp_lcd_touch_del(touch->handles.touch_handle);
        touch->handles.touch_handle = NULL;
    }
    if (touch->owns_io && touch->handles.io_handle != NULL) {
        esp_lcd_panel_io_del(touch->handles.io_handle);
        touch->handles.io_handle = NULL;
    }
    if (touch->owns_i2c_dev && touch->i2c_dev_handle != NULL) {
        i2c_master_bus_rm_device(touch->i2c_dev_handle);
        touch->i2c_dev_handle = NULL;
        touch->owns_i2c_dev = false;
    }
    if (touch->owns_i2c_ref && touch->i2c_name != NULL) {
        esp_board_periph_unref_handle(touch->i2c_name);
        touch->owns_i2c_ref = false;
    }
    free(touch);
}

esp_err_t io_expander_factory_entry_t(i2c_master_bus_handle_t i2c_handle,
                                      const uint16_t dev_addr,
                                      esp_io_expander_handle_t *handle_ret)
{
    esp_err_t ret = esp_io_expander_new_i2c_tca95xx_16bit(i2c_handle, dev_addr, handle_ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create XL9535-compatible IO expander: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t lcd_dsi_panel_factory_entry_t(esp_lcd_dsi_bus_handle_t dsi_handle,
                                        dev_display_lcd_config_t *lcd_cfg,
                                        dev_display_lcd_handles_t *lcd_handles)
{
    dev_display_lcd_config_t local = *lcd_cfg;
    bool is_rm69a10 = detect_rm69a10_panel();
    const lilygo_panel_timing_t *timing = is_rm69a10 ? &s_timing_rm69a10 : &s_timing_hi8561;
    apply_timing(&local, timing);

    esp_lcd_panel_dev_config_t dev_config = {
        .reset_gpio_num = local.sub_cfg.dsi.reset_gpio_num,
        .rgb_ele_order = local.rgb_ele_order,
        .bits_per_pixel = local.bits_per_pixel,
        .data_endian = local.data_endian,
        .flags = {
            .reset_active_high = local.sub_cfg.dsi.reset_active_high,
        },
    };

    if (is_rm69a10) {
        ESP_LOGI(TAG, "T-Display-P4 panel variant: RM69A10");
        rm69a10_vendor_config_t vendor = {
            .dsi_bus = dsi_handle,
            .dpi_config = &local.sub_cfg.dsi.dpi_config,
        };
        dev_config.vendor_config = &vendor;
        return esp_lcd_new_panel_rm69a10(lcd_handles->io_handle, &dev_config,
                                         &lcd_handles->panel_handle);
    }

    ESP_LOGI(TAG, "T-Display-P4 panel variant: HI8561");
    hi8561_vendor_config_t vendor = {
        .dsi_bus = dsi_handle,
        .dpi_config = &local.sub_cfg.dsi.dpi_config,
    };
    dev_config.vendor_config = &vendor;
    return esp_lcd_new_panel_hi8561(lcd_handles->io_handle, &dev_config,
                                    &lcd_handles->panel_handle);
}

esp_err_t lcd_touch_factory_entry_t(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    /* Kept for generated-code compatibility; custom ops handle runtime selection. */
    ESP_RETURN_ON_FALSE(io != NULL && touch_dev_config != NULL && ret_touch != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid touch factory args");
    return esp_lcd_touch_new_i2c_gt9895(io, touch_dev_config, ret_touch);
}

int lilygo_lcd_touch_init(void *cfg, int cfg_size, void **device_handle)
{
    if (!cfg || cfg_size != sizeof(dev_lcd_touch_config_t) || !device_handle) {
        ESP_LOGE(TAG, "Invalid lcd_touch parameters");
        return -1;
    }

    const dev_lcd_touch_config_t *touch_cfg = (const dev_lcd_touch_config_t *)cfg;
    lilygo_lcd_touch_handles_t *touch = calloc(1, sizeof(lilygo_lcd_touch_handles_t));
    if (touch == NULL) {
        ESP_LOGE(TAG, "Failed to allocate lcd_touch handles");
        return -1;
    }

    esp_lcd_touch_config_t touch_config = touch_cfg->touch_config;
    esp_err_t ret = ESP_OK;

    if (detect_rm69a10_panel()) {
        ret = create_gt9895_touch_io(touch_cfg, &touch->handles.io_handle, &touch->i2c_name);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create GT9895 touch IO: %s", esp_err_to_name(ret));
            cleanup_lcd_touch(touch);
            return -1;
        }
        touch->owns_io = true;
        touch->owns_i2c_ref = true;

        ret = esp_lcd_touch_new_i2c_gt9895(touch->handles.io_handle,
                                           &touch_config,
                                           &touch->handles.touch_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create GT9895 touch: %s", esp_err_to_name(ret));
            cleanup_lcd_touch(touch);
            return -1;
        }
        ESP_LOGI(TAG, "Successfully initialized GT9895 LCD touch");
    } else {
        ret = create_hi8561_touch_dev(touch_cfg, &touch->i2c_dev_handle, &touch->i2c_name);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create HI8561 touch device: %s", esp_err_to_name(ret));
            cleanup_lcd_touch(touch);
            return -1;
        }
        touch->owns_i2c_dev = true;
        touch->owns_i2c_ref = true;

        ret = esp_lcd_touch_new_i2c_hi8561(touch->i2c_dev_handle,
                                           &touch_config,
                                           &touch->handles.touch_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create HI8561 touch: %s", esp_err_to_name(ret));
            cleanup_lcd_touch(touch);
            return -1;
        }
        ESP_LOGI(TAG, "Successfully initialized HI8561 LCD touch");
    }

    *device_handle = &touch->handles;
    return 0;
}

int lilygo_lcd_touch_deinit(void *device_handle)
{
    if (device_handle == NULL) {
        return -1;
    }
    cleanup_lcd_touch((lilygo_lcd_touch_handles_t *)device_handle);
    return 0;
}

__attribute__((constructor))
static void register_lilygo_lcd_touch_ops(void)
{
    esp_board_device_set_ops(LILYGO_TOUCH_DEVICE_NAME,
                             lilygo_lcd_touch_init,
                             lilygo_lcd_touch_deinit);
}
