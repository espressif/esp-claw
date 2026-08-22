/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board support for M5Stack StickS3.
 * - M5PM1 PMIC init (powers LCD rail and speaker PA)
 * - ST7789 135×240 display via SPI3, solid-color state rendering
 */
#include "setup_device.h"

/* ESP-IDF headers carry their own `extern "C"` guards. Do NOT wrap them in an
 * extra `extern "C"` — on IDF 5.5 esp_lcd_io_i2c.h (pulled in by
 * esp_lcd_panel_io.h) declares C++ overloads of esp_lcd_new_panel_io_i2c, and
 * forcing C linkage on them is a "conflicting declaration of C function" error. */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_io_spi.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "setup_device";

/* ------------------------------------------------------------------ */
/* Button GPIOs                                                        */
/* ------------------------------------------------------------------ */
#define BTN_A_GPIO  GPIO_NUM_37
#define BTN_B_GPIO  GPIO_NUM_35

/* ------------------------------------------------------------------ */
/* M5PM1 PMIC — internal I2C (SDA=47, SCL=48, addr=0x6E)             */
/* PYG2 = LCD power rail, PYG3 = speaker PA                          */
/* ------------------------------------------------------------------ */
#define M5PM1_ADDR            0x6E
#define M5PM1_I2C_PORT        I2C_NUM_0
#define M5PM1_SDA             GPIO_NUM_47
#define M5PM1_SCL             GPIO_NUM_48
#define M5PM1_REG_I2C_CFG     0x09
#define M5PM1_REG_DIRECTION   0x10
#define M5PM1_REG_OUTPUT      0x11
#define M5PM1_REG_DRIVE_MODE  0x13
#define M5PM1_REG_FUNC_SEL    0x16
#define M5PM1_BIT_LCD_PWR     (1 << 2)
#define M5PM1_BIT_SPK_PA      (1 << 3)

/* ------------------------------------------------------------------ */
/* ST7789 display — SPI3                                               */
/* ------------------------------------------------------------------ */
#define LCD_W           135
#define LCD_H           240
#define LCD_OFFSET_X    52
#define LCD_OFFSET_Y    40
#define LCD_MOSI        GPIO_NUM_39
#define LCD_SCK         GPIO_NUM_40
#define LCD_CS          GPIO_NUM_41
#define LCD_DC          GPIO_NUM_45
#define LCD_RST         GPIO_NUM_21
#define LCD_BL          GPIO_NUM_38
#define LCD_SPI_HOST    SPI3_HOST
#define LCD_SPI_HZ      (40 * 1000 * 1000)

static esp_lcd_panel_handle_t s_panel = NULL;
/* Line buffer for draw_bitmap — same color every pixel, DMA race is harmless */
static uint16_t s_line_buf[LCD_W];

/* ------------------------------------------------------------------ */
/* M5PM1 helpers                                                       */
/* ------------------------------------------------------------------ */
static esp_err_t m5pm1_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, 2, pdMS_TO_TICKS(200));
}

static esp_err_t m5pm1_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, pdMS_TO_TICKS(200));
}

static esp_err_t m5pm1_enable_output(i2c_master_dev_handle_t dev, uint8_t mask)
{
    uint8_t v;
    if (m5pm1_read_reg(dev, M5PM1_REG_FUNC_SEL, &v) == ESP_OK)
        m5pm1_write_reg(dev, M5PM1_REG_FUNC_SEL, v & ~mask);
    if (m5pm1_read_reg(dev, M5PM1_REG_DRIVE_MODE, &v) == ESP_OK)
        m5pm1_write_reg(dev, M5PM1_REG_DRIVE_MODE, v & ~mask);
    if (m5pm1_read_reg(dev, M5PM1_REG_DIRECTION, &v) == ESP_OK)
        m5pm1_write_reg(dev, M5PM1_REG_DIRECTION, v | mask);
    uint8_t out_v;
    esp_err_t err = m5pm1_read_reg(dev, M5PM1_REG_OUTPUT, &out_v);
    if (err != ESP_OK) return err;
    return m5pm1_write_reg(dev, M5PM1_REG_OUTPUT, out_v | mask);
}

static void init_m5pm1(void)
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = M5PM1_I2C_PORT;
    bus_cfg.sda_io_num        = M5PM1_SDA;
    bus_cfg.scl_io_num        = M5PM1_SCL;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err;

    err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "M5PM1: bus create failed: %s", esp_err_to_name(err));
        return;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = M5PM1_ADDR;
    dev_cfg.scl_speed_hz    = 100000;

    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "M5PM1: add device failed: %s", esp_err_to_name(err));
        i2c_del_master_bus(bus);
        return;
    }

    err = m5pm1_write_reg(dev, M5PM1_REG_I2C_CFG, 0x00);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "M5PM1: I2C_CFG failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    if (m5pm1_enable_output(dev, M5PM1_BIT_LCD_PWR) != ESP_OK)
        ESP_LOGW(TAG, "M5PM1: LCD rail failed");
    if (m5pm1_enable_output(dev, M5PM1_BIT_SPK_PA) != ESP_OK)
        ESP_LOGW(TAG, "M5PM1: SPK PA failed");
    ESP_LOGI(TAG, "M5PM1: LCD + SPK rails on");
cleanup:
    i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);
}

/* ------------------------------------------------------------------ */
/* ST7789 init                                                         */
/* ------------------------------------------------------------------ */
static void init_display(void)
{
    /* Backlight GPIO — configure as output and keep off during init */
    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << LCD_BL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(LCD_BL, 0);

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num     = LCD_MOSI;
    bus_cfg.miso_io_num     = -1;
    bus_cfg.sclk_io_num     = LCD_SCK;
    bus_cfg.quadwp_io_num   = -1;
    bus_cfg.quadhd_io_num   = -1;
    bus_cfg.max_transfer_sz = LCD_W * LCD_H * 2;

    if (spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGW(TAG, "LCD: SPI bus init failed");
        return;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num       = LCD_CS;
    io_cfg.dc_gpio_num       = LCD_DC;
    io_cfg.spi_mode          = 0;
    io_cfg.pclk_hz           = LCD_SPI_HZ;
    io_cfg.trans_queue_depth = 4;
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;

    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                  &io_cfg, &io) != ESP_OK) {
        ESP_LOGW(TAG, "LCD: panel IO create failed");
        spi_bus_free(LCD_SPI_HOST);
        return;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num  = LCD_RST;
    panel_cfg.rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.data_endian     = LCD_RGB_DATA_ENDIAN_BIG;
    panel_cfg.bits_per_pixel  = 16;

    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGW(TAG, "LCD: panel create failed");
        esp_lcd_panel_io_del(io);
        spi_bus_free(LCD_SPI_HOST);
        return;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_set_gap(s_panel, LCD_OFFSET_X, LCD_OFFSET_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);

    /* Blank to black before turning on backlight */
    memset(s_line_buf, 0, sizeof(s_line_buf));
    for (int y = 0; y < LCD_H; y++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + 1, s_line_buf);
    }
    gpio_set_level(LCD_BL, 1);

    ESP_LOGI(TAG, "LCD: ST7789 %dx%d ready", LCD_W, LCD_H);
}

/* RGB565 color table per display state */
static uint16_t state_color(rover_s3_display_state_t state)
{
    switch (state) {
    case ROVER_S3_DISPLAY_BOOT:       return 0x4208; /* gray */
    case ROVER_S3_DISPLAY_IDLE:       return 0x000F; /* navy */
    case ROVER_S3_DISPLAY_LISTENING:  return 0x07FF; /* cyan */
    case ROVER_S3_DISPLAY_THINKING:   return 0xFFE0; /* yellow */
    case ROVER_S3_DISPLAY_SPEAKING:   return 0x07E0; /* green */
    case ROVER_S3_DISPLAY_EXECUTING:  return 0xFD20; /* orange */
    case ROVER_S3_DISPLAY_OFFLINE:    return 0xF800; /* red */
    default:                          return 0x0000;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
extern "C" esp_err_t rover_s3_board_init(void)
{
    /* BTN_A/BTN_B gpio_config is intentionally NOT called: GPIO37/35 are the
     * ESP32-S3-PICO-1's internal Octal PSRAM D6/DQS lines on this module.
     * Configuring them as plain GPIO inputs corrupts PSRAM (heap_caps_malloc
     * hangs inside the allocator on the next PSRAM allocation — confirmed via
     * A/B test with the psram_probe() calls in main.c). btn_a/btn_b in
     * board_peripherals.yaml (37/39) don't match this file's constants and
     * GPIO39 is the LCD's own SPI MOSI line, so that mapping needs hardware
     * verification against the real M5StickS3 schematic before buttons can
     * be wired up on a safe pin; until then BTN_A/BTN_B are unavailable. */
    ESP_LOGI(TAG, "board_init: buttons unavailable (GPIO%d/%d reserved by Octal PSRAM)",
             BTN_A_GPIO, BTN_B_GPIO);

    init_m5pm1();
    init_display();

    ESP_LOGI(TAG, "board_init done");
    return ESP_OK;
}

extern "C" void rover_s3_board_update(void) {}

extern "C" bool rover_s3_board_btn_a_pressed(void)
{
    return gpio_get_level(BTN_A_GPIO) == 0;
}

extern "C" bool rover_s3_board_btn_b_pressed(void)
{
    return gpio_get_level(BTN_B_GPIO) == 0;
}

extern "C" int rover_s3_board_get_battery_pct(void)
{
    return -1;
}

extern "C" bool rover_s3_board_is_charging(void)
{
    return false;
}

extern "C" void rover_s3_board_display_state(rover_s3_display_state_t state,
                                               const char *ip, int batt_pct)
{
    (void)ip;
    (void)batt_pct;

    if (!s_panel) {
        static rover_s3_display_state_t last = (rover_s3_display_state_t)-1;
        if (state != last) {
            last = state;
            ESP_LOGI(TAG, "display_state=%d (panel not ready)", (int)state);
        }
        return;
    }

    uint16_t color = state_color(state);
    for (int i = 0; i < LCD_W; i++) s_line_buf[i] = color;
    for (int y = 0; y < LCD_H; y++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + 1, s_line_buf);
    }
}
