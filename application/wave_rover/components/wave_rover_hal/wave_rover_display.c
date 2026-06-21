/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_hal.h"
#include "wave_rover_hal_internal.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wr_display";
static i2c_master_dev_handle_t s_oled_dev = NULL;

#define SSD_CMD  0x00
#define SSD_DATA 0x40

#define SSD_CMD_DISPLAY_ON   0xAF
#define SSD_CMD_DISPLAY_OFF  0xAE

static esp_err_t ssd_cmd(uint8_t cmd)
{
    uint8_t buf[2] = {SSD_CMD, cmd};
    return i2c_master_transmit(s_oled_dev, buf, 2, 50);
}

/* 5×8 font: each char = 5 bytes (columns), MSB at top, ASCII 32–90 */
static const uint8_t s_font5x8[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 32 space */
    {0x00,0x00,0x5F,0x00,0x00}, /* 33 ! */
    {0x00,0x07,0x00,0x07,0x00}, /* 34 " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 35 # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 36 $ */
    {0x23,0x13,0x08,0x64,0x62}, /* 37 % */
    {0x36,0x49,0x55,0x22,0x50}, /* 38 & */
    {0x00,0x05,0x03,0x00,0x00}, /* 39 ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* 40 ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* 41 ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* 42 * */
    {0x08,0x08,0x3E,0x08,0x08}, /* 43 + */
    {0x00,0x50,0x30,0x00,0x00}, /* 44 , */
    {0x08,0x08,0x08,0x08,0x08}, /* 45 - */
    {0x00,0x60,0x60,0x00,0x00}, /* 46 . */
    {0x20,0x10,0x08,0x04,0x02}, /* 47 / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 48 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 49 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 50 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 51 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 52 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 53 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 54 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 55 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 56 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 57 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* 58 : */
    {0x00,0x56,0x36,0x00,0x00}, /* 59 ; */
    {0x08,0x14,0x22,0x41,0x00}, /* 60 < */
    {0x14,0x14,0x14,0x14,0x14}, /* 61 = */
    {0x00,0x41,0x22,0x14,0x08}, /* 62 > */
    {0x02,0x01,0x51,0x09,0x06}, /* 63 ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* 64 @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 65 A */
    {0x7F,0x49,0x49,0x49,0x36}, /* 66 B */
    {0x3E,0x41,0x41,0x41,0x22}, /* 67 C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 68 D */
    {0x7F,0x49,0x49,0x49,0x41}, /* 69 E */
    {0x7F,0x09,0x09,0x09,0x01}, /* 70 F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 71 G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 72 H */
    {0x00,0x41,0x7F,0x41,0x00}, /* 73 I */
    {0x20,0x40,0x41,0x3F,0x01}, /* 74 J */
    {0x7F,0x08,0x14,0x22,0x41}, /* 75 K */
    {0x7F,0x40,0x40,0x40,0x40}, /* 76 L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 77 M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 78 N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 79 O */
    {0x7F,0x09,0x09,0x09,0x06}, /* 80 P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 81 Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* 82 R */
    {0x46,0x49,0x49,0x49,0x31}, /* 83 S */
    {0x01,0x01,0x7F,0x01,0x01}, /* 84 T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 85 U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 86 V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 87 W */
    {0x63,0x14,0x08,0x14,0x63}, /* 88 X */
    {0x07,0x08,0x70,0x08,0x07}, /* 89 Y */
    {0x61,0x51,0x49,0x45,0x43}, /* 90 Z */
};

static void ssd_set_cursor(int col, int page)
{
    ssd_cmd(0xB0 | (page & 0x07));
    ssd_cmd(0x00 | (col & 0x0F));
    ssd_cmd(0x10 | ((col >> 4) & 0x0F));
}

static void ssd_print_char(uint8_t c)
{
    if (c < 32 || c > 90) c = 32;
    const uint8_t *glyph = s_font5x8[c - 32];
    uint8_t buf[7] = {SSD_DATA, glyph[0], glyph[1], glyph[2], glyph[3], glyph[4], 0x00};
    i2c_master_transmit(s_oled_dev, buf, 7, 50);
}

static esp_err_t oled_init(void)
{
    if (!g_wr_i2c_bus) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = WR_OLED_ADDR,
        .scl_speed_hz    = WR_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(g_wr_i2c_bus, &dev_cfg, &s_oled_dev),
                        TAG, "add ssd1306");

    static const uint8_t init_seq[] = {
        0xAE,       /* display off */
        0x20, 0x00, /* horizontal addressing */
        0xB0,       /* page start */
        0xC8,       /* COM output scan dir */
        0x00,       /* low col addr */
        0x10,       /* high col addr */
        0x40,       /* start line 0 */
        0x81, 0x7F, /* contrast */
        0xA1,       /* seg remap */
        0xA6,       /* normal display */
        0xA8, 0x1F, /* mux 32 lines */
        0xA4,       /* output follows RAM */
        0xD3, 0x00, /* display offset 0 */
        0xD5, 0xF0, /* clock */
        0xD9, 0x22, /* precharge */
        0xDA, 0x02, /* COM pins for 32px height */
        0xDB, 0x20, /* vcomh */
        0x8D, 0x14, /* charge pump on */
        0xAF,       /* display on */
    };
    for (size_t i = 0; i < sizeof(init_seq); i++) {
        ssd_cmd(init_seq[i]);
    }
    ESP_LOGI(TAG, "SSD1306 initialized at 0x%02X", WR_OLED_ADDR);
    return ESP_OK;
}

esp_err_t wr_display_clear(void)
{
    if (!g_wr_i2c_bus) return ESP_OK;
    if (!s_oled_dev) {
        if (oled_init() != ESP_OK) return ESP_OK;
    }
    ssd_cmd(0x21); ssd_cmd(0); ssd_cmd(127);
    ssd_cmd(0x22); ssd_cmd(0); ssd_cmd(3);
    uint8_t buf[17];
    buf[0] = SSD_DATA;
    memset(buf + 1, 0, 16);
    for (int i = 0; i < 32; i++) {
        i2c_master_transmit(s_oled_dev, buf, 17, 50);
    }
    return ESP_OK;
}

esp_err_t wr_display_text(const char *text, int line, bool clear_first)
{
    if (!g_wr_i2c_bus) return ESP_OK;
    if (!s_oled_dev) {
        if (oled_init() != ESP_OK) return ESP_OK;
    }
    if (clear_first) wr_display_clear();
    if (!text) return ESP_OK;

    int page = line & 0x03;
    ssd_set_cursor(0, page);
    for (int i = 0; text[i] && i < 21; i++) {
        ssd_print_char((uint8_t)text[i]);
    }
    return ESP_OK;
}

esp_err_t wr_display_set_power(bool on)
{
    if (!g_wr_i2c_bus || !s_oled_dev) {
        ESP_LOGD(TAG, "display_set_power: display not ready, skipping");
        return ESP_OK;
    }
    return ssd_cmd(on ? SSD_CMD_DISPLAY_ON : SSD_CMD_DISPLAY_OFF);
}

esp_err_t wr_display_status(const char *fw_ver, const char *wifi_info,
                             float battery_v, bool mcp_active, bool estop)
{
    char line[22];
    wr_display_clear();
    snprintf(line, sizeof(line), "FW:%s", fw_ver ? fw_ver : "?");
    wr_display_text(line, 0, false);
    snprintf(line, sizeof(line), "%.16s", wifi_info ? wifi_info : "no wifi");
    wr_display_text(line, 1, false);
    snprintf(line, sizeof(line), "BATT:%.1fV", battery_v);
    wr_display_text(line, 2, false);
    snprintf(line, sizeof(line), "MCP:%s%s", mcp_active ? "ON" : "OFF",
             estop ? " ESTOP" : "");
    wr_display_text(line, 3, false);
    return ESP_OK;
}
