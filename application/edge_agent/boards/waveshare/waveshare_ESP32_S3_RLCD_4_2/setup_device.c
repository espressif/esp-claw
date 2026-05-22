/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file setup_device.c
 * @brief Waveshare ESP32-S3-RLCD-4.2 custom device initialisation.
 *
 * Pin mapping (from hardware schematic / Waveshare reference):
 *   MOSI : GPIO12    SCK  : GPIO11
 *   CS   : GPIO40    DC   : GPIO5
 *   RST  : GPIO41    TE   : GPIO6  (TE line not used in software)
 *
 * Display geometry: 400 × 300 pixels (landscape, width = 400, height = 300)
 *
 * Frame-buffer layout (1 bpp, landscape):
 *   For pixel (x, y):
 *     inv_y      = (HEIGHT - 1) - y
 *     block_y    = inv_y / 4              (which 4-row group, 0..74)
 *     local_y    = inv_y % 4              (row within group, 0..3)
 *     byte_x     = x / 2                 (which column pair, 0..199)
 *     local_x    = x % 2                 (column within pair, 0..1)
 *     byte_index = byte_x * H4 + block_y (H4 = HEIGHT/4 = 75)
 *     bit        = 7 - (local_y * 2 + local_x)
 *   Total frame-buffer size: 400 * 300 / 8 = 15 000 bytes
 *
 * Frame-buffer placement:
 *   SPIRAM is preferred (keeps internal SRAM free for other uses).  If
 *   SPIRAM is unavailable (e.g. CONFIG_SPIRAM not set in sdkconfig),
 *   the buffer falls back to DMA-capable internal SRAM.  On ESP32-S3 the
 *   GDMA controller can reach both memory regions, so SPI transfers work
 *   in either case.
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_bit_defs.h"
#include "esp_lcd_panel_interface.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gen_board_device_custom.h"

static const char *TAG = "waveshare_rlcd_4_2";

/* ── Pin assignments ─────────────────────────────────────────────────────── */
#define RLCD_MOSI_GPIO 12
#define RLCD_SCK_GPIO 11
#define RLCD_CS_GPIO 40
#define RLCD_DC_GPIO 5
#define RLCD_RST_GPIO 41

/* ── Display geometry ────────────────────────────────────────────────────── */
#define RLCD_WIDTH 400
#define RLCD_HEIGHT 300

/* ── SPI ─────────────────────────────────────────────────────────────────── */
#define RLCD_SPI_HOST SPI3_HOST
#define RLCD_SPI_CLK_HZ (40 * 1000 * 1000)

/* ── Frame-buffer ────────────────────────────────────────────────────────── */
/* 1 bpp, landscape: 400 * 300 / 8 = 15 000 bytes                           */
#define RLCD_FRAME_BUF_BYTES ((RLCD_WIDTH) * (RLCD_HEIGHT) / 8)
/* H4 = HEIGHT / 4 = 75; part of the byte-index formula                     */
#define RLCD_H4 ((RLCD_HEIGHT) / 4)
/*
 * Luma threshold for binarisation
 */
#define RLCD_LUMA_THRESHOLD 130

/* ── Custom panel context ────────────────────────────────────────────────── */
typedef struct
{
    esp_lcd_panel_t base; /* Must be first – direct cast target */
    esp_lcd_panel_io_handle_t io;
    gpio_num_t rst_gpio;
    uint8_t *frame_buf; /* 1-bpp frame buffer (SPIRAM preferred, or internal DMA RAM) */
} rlcd_panel_t;

/* ── Initialisation-command table ────────────────────────────────────────── */
#define RLCD_INIT_MAX_DATA 10

typedef struct
{
    uint8_t cmd;
    uint8_t data[RLCD_INIT_MAX_DATA];
    uint8_t len;       /* number of valid data bytes  */
    uint32_t delay_ms; /* post-command delay (0 = no delay) */
} rlcd_init_cmd_t;

/*
 * Initialisation sequence derived from the Waveshare ESP32-S3-RLCD-4.2
 * reference implementation (xiaozhi-esp32 / CustomLcdDisplay::RLCD_Init).
 */
static const rlcd_init_cmd_t s_rlcd_init_cmds[] = {
    {0xD6, {0x17, 0x02}, 2, 0},
    {0xD1, {0x01}, 1, 0},
    {0xC0, {0x11, 0x04}, 2, 0},
    {0xC1, {0x69, 0x69, 0x69, 0x69}, 4, 0},
    {0xC2, {0x19, 0x19, 0x19, 0x19}, 4, 0},
    {0xC4, {0x4B, 0x4B, 0x4B, 0x4B}, 4, 0},
    {0xC5, {0x19, 0x19, 0x19, 0x19}, 4, 0},
    {0xD8, {0x80, 0xE9}, 2, 0},
    {0xB2, {0x02}, 1, 0},
    {0xB3, {0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45}, 10, 0},
    {0xB4, {0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45}, 8, 0},
    {0x62, {0x32, 0x03, 0x1F}, 3, 0},
    {0xB7, {0x13}, 1, 0},
    {0xB0, {0x64}, 1, 0},
    {0x11, {0x00}, 0, 200}, /* Sleep Out – wait 200 ms */
    {0xC9, {0x00}, 1, 0},
    {0x36, {0x48}, 1, 0},
    {0x3A, {0x11}, 1, 0}, /* Pixel format: 1 bpp */
    {0xB9, {0x20}, 1, 0},
    {0xB8, {0x29}, 1, 0},
    {0x21, {0x00}, 0, 0},       /* Display Inversion On */
    {0x2A, {0x12, 0x2A}, 2, 0}, /* Column Address Set */
    {0x2B, {0x00, 0xC7}, 2, 0}, /* Page Address Set */
    {0x35, {0x00}, 1, 0},       /* Tearing Effect Line On */
    {0xD0, {0xFF}, 1, 0},
    {0x38, {0x00}, 0, 0}, /* Exit Idle Mode */
    {0x29, {0x00}, 0, 0}, /* Display On */
};

/* ── Frame flush ─────────────────────────────────────────────────────────── */

static esp_err_t rlcd_flush_frame(rlcd_panel_t *rlcd)
{
    esp_err_t ret;
    uint8_t col_data[2] = {0x12, 0x2A};
    uint8_t page_data[2] = {0x00, 0xC7};

    ret = esp_lcd_panel_io_tx_param(rlcd->io, 0x2A, col_data, sizeof(col_data));
    ESP_RETURN_ON_ERROR(ret, TAG, "col addr set failed");

    ret = esp_lcd_panel_io_tx_param(rlcd->io, 0x2B, page_data, sizeof(page_data));
    ESP_RETURN_ON_ERROR(ret, TAG, "page addr set failed");

    /* 0x2C = Memory Write command; frame data follows as the colour payload */
    ret = esp_lcd_panel_io_tx_color(rlcd->io, 0x2C, rlcd->frame_buf,
                                    RLCD_FRAME_BUF_BYTES);
    ESP_RETURN_ON_ERROR(ret, TAG, "frame flush failed");

    return ESP_OK;
}

/* ── esp_lcd_panel_t operations ──────────────────────────────────────────── */

static esp_err_t rlcd_panel_reset(esp_lcd_panel_t *panel)
{
    rlcd_panel_t *rlcd = (rlcd_panel_t *)panel;
    gpio_set_level(rlcd->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(rlcd->rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(rlcd->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static esp_err_t rlcd_panel_init(esp_lcd_panel_t *panel)
{
    rlcd_panel_t *rlcd = (rlcd_panel_t *)panel;
    esp_err_t ret;
    uint8_t data_buf[RLCD_INIT_MAX_DATA];

    for (size_t i = 0; i < sizeof(s_rlcd_init_cmds) / sizeof(s_rlcd_init_cmds[0]); i++)
    {
        const rlcd_init_cmd_t *c = &s_rlcd_init_cmds[i];
        /* Copy to SRAM so the SPI DMA engine can reach the data */
        if (c->len > 0)
        {
            memcpy(data_buf, c->data, c->len);
        }
        ret = esp_lcd_panel_io_tx_param(rlcd->io, c->cmd,
                                        c->len > 0 ? data_buf : NULL, c->len);
        ESP_RETURN_ON_ERROR(ret, TAG, "init cmd 0x%02X failed", c->cmd);
        if (c->delay_ms > 0)
        {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }

    /* Clear frame-buffer to white (all bits set) */
    memset(rlcd->frame_buf, 0xFF, RLCD_FRAME_BUF_BYTES);

    ESP_LOGI(TAG, "RLCD initialised (%d×%d)", RLCD_WIDTH, RLCD_HEIGHT);
    return ESP_OK;
}

static esp_err_t rlcd_panel_del(esp_lcd_panel_t *panel)
{
    rlcd_panel_t *rlcd = (rlcd_panel_t *)panel;
    if (rlcd->frame_buf != NULL)
    {
        free(rlcd->frame_buf);
        rlcd->frame_buf = NULL;
    }
    free(rlcd);
    return ESP_OK;
}

/**
 * @brief Convert an RGB565 dirty rectangle to the 1-bpp frame-buffer using
 *        BT.601-luma threshold binarization, then flush the complete frame
 *        to the panel.
 *
 * Algorithm (per pixel):
 *   1. Unpack RGB565 → 8-bit R, G, B (replicate MSBs into LSBs).
 *   2. Compute perceptual luma: L = (R8×77 + G8×150 + B8×29) >> 8
 *      (integer BT.601 approximation, result in [0, 255]).
 *   3. Binarization
 *
 * x_end / y_end are exclusive (ESP-IDF LCD panel API convention).
 */
static esp_err_t rlcd_panel_draw_bitmap(esp_lcd_panel_t *panel,
                                        int x_start, int y_start,
                                        int x_end, int y_end,
                                        const void *color_data)
{
    rlcd_panel_t *rlcd = (rlcd_panel_t *)panel;

    /* Clamp to display bounds */
    if (x_start < 0)
    {
        x_start = 0;
    }
    if (y_start < 0)
    {
        y_start = 0;
    }
    if (x_end > RLCD_WIDTH)
    {
        x_end = RLCD_WIDTH;
    }
    if (y_end > RLCD_HEIGHT)
    {
        y_end = RLCD_HEIGHT;
    }
    if (x_start >= x_end || y_start >= y_end)
    {
        return ESP_OK;
    }

    const uint16_t *src = (const uint16_t *)color_data;
    const int w = x_end - x_start;

    for (int y = y_start; y < y_end; y++)
    {
        const int inv_y = (RLCD_HEIGHT - 1) - y;
        const int block_y = inv_y >> 2; /* which 4-row group (0..H4-1) */
        const int local_y = inv_y & 3;  /* row within that group (0..3) */

        for (int xi = 0; xi < w; xi++)
        {
            const int x = x_start + xi;

            /* Unpack RGB565 to 8-bit channels */
            const uint16_t p = src[(y - y_start) * w + xi];
            const int r5 = (p >> 11) & 0x1F;
            const int g6 = (p >> 5) & 0x3F;
            const int b5 = p & 0x1F;
            /* Expand to 8 bits (replicate MSBs into LSBs) */
            const int r8 = (r5 << 3) | (r5 >> 2);
            const int g8 = (g6 << 2) | (g6 >> 4);
            const int b8 = (b5 << 3) | (b5 >> 2);

            /* BT.601 luma: 0.299*R + 0.587*G + 0.114*B (integer, 0–255) */
            const int luma = (r8 * 77 + g8 * 150 + b8 * 29) >> 8;
            const int white = (luma >= RLCD_LUMA_THRESHOLD) ? 1 : 0;

            /* Write into 1-bpp frame buffer */
            const int byte_x = x >> 1;
            const int local_x = x & 1;
            const uint32_t idx = (uint32_t)byte_x * RLCD_H4 + (uint32_t)block_y;
            const uint8_t mask = (uint8_t)(1u << (7 - ((local_y << 1) | local_x)));
            if (white)
            {
                rlcd->frame_buf[idx] |= mask;
            }
            else
            {
                rlcd->frame_buf[idx] &= ~mask;
            }
        }
    }

    return rlcd_flush_frame(rlcd);
}

/* Stub operations – not required for this monochrome reflective panel */
static esp_err_t rlcd_panel_mirror(esp_lcd_panel_t *p, bool mx, bool my)
{
    (void)p;
    (void)mx;
    (void)my;
    return ESP_OK;
}
static esp_err_t rlcd_panel_swap_xy(esp_lcd_panel_t *p, bool swap)
{
    (void)p;
    (void)swap;
    return ESP_OK;
}
static esp_err_t rlcd_panel_set_gap(esp_lcd_panel_t *p, int xg, int yg)
{
    (void)p;
    (void)xg;
    (void)yg;
    return ESP_OK;
}
static esp_err_t rlcd_panel_invert_color(esp_lcd_panel_t *p, bool inv)
{
    (void)p;
    (void)inv;
    return ESP_OK;
}
static esp_err_t rlcd_panel_disp_on_off(esp_lcd_panel_t *p, bool on)
{
    (void)p;
    (void)on;
    return ESP_OK;
}
static esp_err_t rlcd_panel_disp_sleep(esp_lcd_panel_t *p, bool sleep)
{
    (void)p;
    (void)sleep;
    return ESP_OK;
}

/* ── dev_display_lcd_config reported to esp_board_manager ───────────────── */

/*
 * bits_per_pixel = 16 so that LVGL allocates RGB565 render buffers.
 * The panel's draw_bitmap converts RGB565 → 1 bpp internally.
 */
static const dev_display_lcd_config_t s_lcd_config = {
    .name = "display_lcd",
    .chip = "rlcd",
    .sub_type = "spi",
    .lcd_width = RLCD_WIDTH,
    .lcd_height = RLCD_HEIGHT,
    .swap_xy = 0,
    .mirror_x = 0,
    .mirror_y = 0,
    .invert_color = 0,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    .bits_per_pixel = 16,
};

/* ── Persistent device state ─────────────────────────────────────────────── */
static dev_display_lcd_handles_t s_lcd_handles;

/* ── Custom device lifecycle ─────────────────────────────────────────────── */

static int display_lcd_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "device_handle is NULL");

    esp_err_t ret = ESP_OK;

    /* ── 1. Allocate 1-bpp frame buffer ──────────────────────────────────
     * Prefer SPIRAM to conserve internal SRAM; fall back to DMA-capable
     * internal RAM when CONFIG_SPIRAM is not set or SPIRAM heap is empty.
     * Both regions are accessible by the ESP32-S3 GDMA controller, so the
     * SPI flush in rlcd_flush_frame() works from either location.         */
    uint8_t *frame_buf = (uint8_t *)heap_caps_calloc(1, RLCD_FRAME_BUF_BYTES,
                                                     MALLOC_CAP_SPIRAM);
    if (frame_buf == NULL)
    {
        ESP_LOGW(TAG, "SPIRAM unavailable, allocating frame-buffer from internal DMA RAM");
        frame_buf = (uint8_t *)heap_caps_calloc(1, RLCD_FRAME_BUF_BYTES,
                                                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    ESP_RETURN_ON_FALSE(frame_buf != NULL, ESP_ERR_NO_MEM, TAG,
                        "frame-buffer alloc failed");

    /* ── 2. Configure RST GPIO ────────────────────────────────────────── */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = BIT64(RLCD_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&rst_cfg);
    ESP_GOTO_ON_ERROR(ret, err_free_fb, TAG, "RST GPIO config failed");

    /* ── 3. Initialise SPI bus ────────────────────────────────────────── */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RLCD_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = RLCD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = RLCD_FRAME_BUF_BYTES + 16,
    };
    ret = spi_bus_initialize(RLCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    ESP_GOTO_ON_ERROR(ret, err_free_fb, TAG, "SPI bus init failed");

    /* ── 4. Create panel IO (SPI) ─────────────────────────────────────── */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = RLCD_DC_GPIO,
        .cs_gpio_num = RLCD_CS_GPIO,
        .pclk_hz = RLCD_SPI_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)RLCD_SPI_HOST,
                                   &io_cfg, &io_handle);
    ESP_GOTO_ON_ERROR(ret, err_free_spi, TAG, "panel IO init failed");

    /* ── 5. Allocate and populate custom panel object ─────────────────── */
    rlcd_panel_t *rlcd = (rlcd_panel_t *)calloc(1, sizeof(rlcd_panel_t));
    ESP_GOTO_ON_FALSE(rlcd != NULL, ESP_ERR_NO_MEM, err_free_io, TAG,
                      "panel alloc failed");

    rlcd->io = io_handle;
    rlcd->rst_gpio = (gpio_num_t)RLCD_RST_GPIO;
    rlcd->frame_buf = frame_buf;

    rlcd->base.reset = rlcd_panel_reset;
    rlcd->base.init = rlcd_panel_init;
    rlcd->base.del = rlcd_panel_del;
    rlcd->base.draw_bitmap = rlcd_panel_draw_bitmap;
    rlcd->base.mirror = rlcd_panel_mirror;
    rlcd->base.swap_xy = rlcd_panel_swap_xy;
    rlcd->base.set_gap = rlcd_panel_set_gap;
    rlcd->base.invert_color = rlcd_panel_invert_color;
    rlcd->base.disp_on_off = rlcd_panel_disp_on_off;
    rlcd->base.disp_sleep = rlcd_panel_disp_sleep;

    /* ── 6. Reset and initialise the panel ───────────────────────────── */
    ret = esp_lcd_panel_reset(&rlcd->base);
    ESP_GOTO_ON_ERROR(ret, err_free_panel, TAG, "panel reset failed");

    ret = esp_lcd_panel_init(&rlcd->base);
    ESP_GOTO_ON_ERROR(ret, err_free_panel, TAG, "panel init failed");

    /* ── 7. Register with esp_board_manager ──────────────────────────── */
    s_lcd_handles.io_handle = io_handle;
    s_lcd_handles.panel_handle = &rlcd->base;
    esp_board_device_override_config("display_lcd", &s_lcd_config, sizeof(s_lcd_config));
    *device_handle = &s_lcd_handles;

    ESP_LOGI(TAG, "RLCD-4.2 ready (%d×%d @ %d MHz)",
             RLCD_WIDTH, RLCD_HEIGHT, RLCD_SPI_CLK_HZ / 1000000);
    return ESP_OK;

err_free_panel:
    free(rlcd); /* frame_buf freed below via err_free_fb */
err_free_io:
    esp_lcd_panel_io_del(io_handle);
err_free_spi:
    spi_bus_free(RLCD_SPI_HOST);
err_free_fb:
    free(frame_buf);
    return ret;
}

static int display_lcd_deinit(void *device_handle)
{
    dev_display_lcd_handles_t *handles = (dev_display_lcd_handles_t *)device_handle;
    if (handles != NULL)
    {
        if (handles->panel_handle != NULL)
        {
            esp_lcd_panel_del(handles->panel_handle);
            handles->panel_handle = NULL;
        }
        if (handles->io_handle != NULL)
        {
            esp_lcd_panel_io_del(handles->io_handle);
            handles->io_handle = NULL;
        }
    }
    spi_bus_free(RLCD_SPI_HOST);
    ESP_LOGI(TAG, "RLCD-4.2 deinitialised");
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(display_lcd, display_lcd_init, display_lcd_deinit);
