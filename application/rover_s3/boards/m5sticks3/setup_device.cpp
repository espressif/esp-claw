/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board support for M5Stack StickS3.
 * Initialises M5PM1 PMIC to power LCD and speaker PA rails.
 * Display rendering is stubbed — add ST7789 / esp_lcd later.
 */
#include "setup_device.h"

extern "C" {
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

static const char *TAG = "setup_device";

/* StickS3 button GPIOs */
#define BTN_A_GPIO  GPIO_NUM_37
#define BTN_B_GPIO  GPIO_NUM_35

/* M5PM1 PMIC — internal I2C bus (SDA=47, SCL=48) */
#define M5PM1_ADDR            0x6E
#define M5PM1_I2C_PORT        I2C_NUM_0
#define M5PM1_SDA             GPIO_NUM_47
#define M5PM1_SCL             GPIO_NUM_48
#define M5PM1_REG_I2C_CFG     0x09
#define M5PM1_REG_DIRECTION   0x10
#define M5PM1_REG_OUTPUT      0x11
#define M5PM1_REG_DRIVE_MODE  0x13
#define M5PM1_REG_FUNC_SEL    0x16
#define M5PM1_BIT_LCD_PWR     (1 << 2)   /* PYG2: LCD / L3B power */
#define M5PM1_BIT_SPK_PA      (1 << 3)   /* PYG3: speaker amplifier */

static esp_err_t m5pm1_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, 2, pdMS_TO_TICKS(200));
}

static esp_err_t m5pm1_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, pdMS_TO_TICKS(200));
}

/* Configure one PMIC GPIO channel as push-pull output driven high */
static esp_err_t m5pm1_enable_output(i2c_master_dev_handle_t dev, uint8_t mask)
{
    uint8_t v;
    /* FUNC_SEL = 0 → generic GPIO */
    if (m5pm1_read_reg(dev, M5PM1_REG_FUNC_SEL, &v) == ESP_OK)
        m5pm1_write_reg(dev, M5PM1_REG_FUNC_SEL, v & ~mask);
    /* DRIVE_MODE = 0 → push-pull */
    if (m5pm1_read_reg(dev, M5PM1_REG_DRIVE_MODE, &v) == ESP_OK)
        m5pm1_write_reg(dev, M5PM1_REG_DRIVE_MODE, v & ~mask);
    /* DIRECTION = 1 → output */
    if (m5pm1_read_reg(dev, M5PM1_REG_DIRECTION, &v) == ESP_OK)
        m5pm1_write_reg(dev, M5PM1_REG_DIRECTION, v | mask);
    /* OUTPUT = 1 → high */
    uint8_t out_v;
    esp_err_t err = m5pm1_read_reg(dev, M5PM1_REG_OUTPUT, &out_v);
    if (err != ESP_OK) return err;
    return m5pm1_write_reg(dev, M5PM1_REG_OUTPUT, out_v | mask);
}

/*
 * Create a temporary I2C bus, enable M5PM1 power rails, then delete the bus.
 * M5PM1 retains register state after the bus is released.
 * cap_voice_audio will later open its own bus on the same port.
 */
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
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "M5PM1: bus create failed");
        return;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = M5PM1_ADDR;
    dev_cfg.scl_speed_hz    = 100000;

    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "M5PM1: add device failed");
        i2c_del_master_bus(bus);
        return;
    }

    /* Disable PMIC idle sleep before further register access */
    esp_err_t err = m5pm1_write_reg(dev, M5PM1_REG_I2C_CFG, 0x00);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "M5PM1: I2C_CFG write failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    if (m5pm1_enable_output(dev, M5PM1_BIT_LCD_PWR) != ESP_OK) {
        ESP_LOGW(TAG, "M5PM1: LCD rail enable failed");
    }
    if (m5pm1_enable_output(dev, M5PM1_BIT_SPK_PA) != ESP_OK) {
        ESP_LOGW(TAG, "M5PM1: SPK PA enable failed");
    }
    ESP_LOGI(TAG, "M5PM1: LCD + SPK rails on");

cleanup:
    i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);
}

extern "C" esp_err_t rover_s3_board_init(void)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_A_GPIO) | (1ULL << BTN_B_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&btn_cfg);
    if (err != ESP_OK) return err;

    init_m5pm1();

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
    /* Display stub — ST7789 rendering to be added */
    static rover_s3_display_state_t last_state = (rover_s3_display_state_t)-1;
    if (state != last_state) {
        last_state = state;
        ESP_LOGI(TAG, "display_state=%d ip=%s batt=%d%%",
                 (int)state, ip ? ip : "--", batt_pct);
    }
}
