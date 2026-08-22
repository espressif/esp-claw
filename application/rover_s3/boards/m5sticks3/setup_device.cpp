/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board support for M5Stack StickS3.
 * - M5PM1 PMIC init (powers LCD rail and speaker PA)
 * - ST7789 135×240 display via M5GFX (LGFX_Device), state UI rendering
 */
#include "setup_device.h"

/* ESP-IDF headers carry their own `extern "C"` guards. Do NOT wrap them in an
 * extra `extern "C"` — on IDF 5.5 esp_lcd_io_i2c.h (pulled in by
 * esp_lcd_panel_io.h) declares C++ overloads of esp_lcd_new_panel_io_i2c, and
 * forcing C linkage on them is a "conflicting declaration of C function" error. */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_ST7789.hpp>
#include <lgfx/v1/platforms/esp32/Bus_SPI.hpp>
#include <lgfx/v1/platforms/esp32/Light_PWM.hpp>
#include <string.h>

static const char *TAG = "setup_device";

/* ------------------------------------------------------------------ */
/* Button GPIOs — M5StickS3 BtnA/BtnB per M5Unified's authoritative    */
/* board table. NOT GPIO37/35 (those are this module's internal Octal  */
/* PSRAM D6/DQS lines — see rover_s3_board_init() comment below) and    */
/* NOT the 37/39 in board_peripherals.yaml (also wrong; 39 is LCD MOSI).*/
/* ------------------------------------------------------------------ */
#define BTN_A_GPIO  GPIO_NUM_11
#define BTN_B_GPIO  GPIO_NUM_12

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
/* ST7789 display — SPI3, driven directly via a hand-configured        */
/* LGFX_Device instead of M5GFX's autodetect()/M5Unified's M5.begin(). */
/* Both of those probe OTHER M5 boards first (M5StackCoreS3's block    */
/* reconfigures GPIO35/36/37 for its SPI bus), which would corrupt     */
/* this module's Octal PSRAM the same way the old BTN_A/BTN_B          */
/* gpio_config() did. Pinning the exact M5StickS3 panel config here    */
/* (values verified against M5GFX's own board_M5StickS3 case in        */
/* M5GFX.cpp) skips probing entirely. M5Unified itself is not used at  */
/* all: its board_M5StickS3 case also clears PM1 gpio3 (speaker PA),   */
/* which would silence the speaker our own init_m5pm1() enables below. */
/* ------------------------------------------------------------------ */
class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789 _panel_instance;
    lgfx::Bus_SPI      _bus_instance;
    lgfx::Light_PWM    _light_instance;

public:
    LGFX(void)
    {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host    = SPI3_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = true;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = GPIO_NUM_40;
            cfg.pin_mosi    = GPIO_NUM_39;
            cfg.pin_miso    = -1;
            cfg.pin_dc      = GPIO_NUM_45;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = GPIO_NUM_41;
            cfg.pin_rst          = GPIO_NUM_21;
            cfg.pin_busy         = -1;
            cfg.panel_width      = 135;
            cfg.panel_height     = 240;
            cfg.offset_x         = 52;
            cfg.offset_y         = 40;
            cfg.offset_rotation  = 0;
            cfg.readable         = true;
            cfg.invert           = true;
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;
            _panel_instance.config(cfg);
        }
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl      = GPIO_NUM_38;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }
        setPanel(&_panel_instance);
    }
};

static LGFX s_lcd;
static bool s_lcd_ready = false;

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
/* Buttons                                                             */
/* ------------------------------------------------------------------ */
static void init_buttons(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BTN_A_GPIO) | (1ULL << BTN_B_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

/* ------------------------------------------------------------------ */
/* ST7789 init                                                         */
/* ------------------------------------------------------------------ */
static void init_display(void)
{
    if (!s_lcd.init()) {
        ESP_LOGW(TAG, "LCD: init failed");
        return;
    }
    s_lcd.setRotation(0);
    s_lcd.setBrightness(128);
    s_lcd.fillScreen(TFT_BLACK);
    s_lcd_ready = true;
    ESP_LOGI(TAG, "LCD: ST7789 %dx%d ready", s_lcd.width(), s_lcd.height());
}

struct state_style_t {
    const char *label;
    uint8_t r, g, b;
};

static state_style_t state_style(rover_s3_display_state_t state)
{
    switch (state) {
    case ROVER_S3_DISPLAY_BOOT:      return { "BOOT",    0x40, 0x40, 0x40 };
    case ROVER_S3_DISPLAY_IDLE:      return { "IDLE",    0x00, 0x00, 0x80 };
    case ROVER_S3_DISPLAY_LISTENING: return { "LISTEN",  0x00, 0xC0, 0xC0 };
    case ROVER_S3_DISPLAY_THINKING:  return { "THINK",   0xC0, 0xC0, 0x00 };
    case ROVER_S3_DISPLAY_SPEAKING:  return { "SPEAK",   0x00, 0xA0, 0x00 };
    case ROVER_S3_DISPLAY_EXECUTING: return { "EXEC",    0xE0, 0x80, 0x00 };
    case ROVER_S3_DISPLAY_OFFLINE:   return { "OFFLINE", 0xC0, 0x00, 0x00 };
    default:                         return { "?",       0x00, 0x00, 0x00 };
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
extern "C" esp_err_t rover_s3_board_init(void)
{
    init_m5pm1();
    init_display();
    init_buttons();

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
    if (!s_lcd_ready) {
        static rover_s3_display_state_t last = (rover_s3_display_state_t)-1;
        if (state != last) {
            last = state;
            ESP_LOGI(TAG, "display_state=%d (panel not ready)", (int)state);
        }
        return;
    }

    state_style_t style = state_style(state);
    uint16_t header_color = s_lcd.color565(style.r, style.g, style.b);

    s_lcd.startWrite();
    s_lcd.fillScreen(TFT_BLACK);
    s_lcd.fillRect(0, 0, s_lcd.width(), 40, header_color);

    s_lcd.setTextColor(TFT_WHITE, header_color);
    s_lcd.setTextSize(2);
    s_lcd.setCursor(6, 12);
    s_lcd.print(style.label);

    s_lcd.setTextSize(1);
    s_lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    s_lcd.setCursor(4, 52);
    s_lcd.printf("IP: %s", (ip && ip[0]) ? ip : "---");

    s_lcd.setCursor(4, 68);
    if (batt_pct >= 0) {
        s_lcd.printf("BAT: %d%%", batt_pct);
    } else {
        s_lcd.print("BAT: --");
    }

    s_lcd.endWrite();
}
