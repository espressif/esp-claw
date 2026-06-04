/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>

#include "esp_check.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_rm69a10.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "esp_lcd_rm69a10";

#define RM69A10_CMD_SLPIN      0x10
#define RM69A10_CMD_SLPOUT     0x11
#define RM69A10_CMD_INVOFF     0x20
#define RM69A10_CMD_INVON      0x21
#define RM69A10_CMD_DISPOFF    0x28
#define RM69A10_CMD_DISPON     0x29
#define RM69A10_CMD_WRDISBV    0x51

typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t data_len;
    uint16_t delay_ms;
} rm69a10_lcd_init_cmd_t;

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_handle_t dpi_panel;
    esp_lcd_panel_io_handle_t io;
} rm69a10_panel_t;

static esp_err_t panel_rm69a10_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_rm69a10_init(esp_lcd_panel_t *panel);
static esp_err_t panel_rm69a10_del(esp_lcd_panel_t *panel);
static esp_err_t panel_rm69a10_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data);
static esp_err_t panel_rm69a10_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_rm69a10_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_rm69a10_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_rm69a10_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_rm69a10_disp_on_off(esp_lcd_panel_t *panel, bool on_off);
static esp_err_t panel_rm69a10_disp_sleep(esp_lcd_panel_t *panel, bool sleep);
static esp_err_t panel_rm69a10_set_brightness(esp_lcd_panel_t *panel, int brightness);

static const rm69a10_lcd_init_cmd_t s_rm69a10_init_cmds[] = {
    {0xfe, (const uint8_t[]){0xfd}, sizeof((const uint8_t[]){0xfd}), 0},
    {0x80, (const uint8_t[]){0xfc}, sizeof((const uint8_t[]){0xfc}), 0},
    {0xfe, (const uint8_t[]){0x00}, sizeof((const uint8_t[]){0x00}), 0},
    {0x2a, (const uint8_t[]){0x00, 0x00, 0x02, 0x37}, sizeof((const uint8_t[]){0x00, 0x00, 0x02, 0x37}), 0},
    {0x2b, (const uint8_t[]){0x00, 0x00, 0x04, 0xcf}, sizeof((const uint8_t[]){0x00, 0x00, 0x04, 0xcf}), 0},
    {0x31, (const uint8_t[]){0x00, 0x03, 0x02, 0x34}, sizeof((const uint8_t[]){0x00, 0x03, 0x02, 0x34}), 0},
    {0x30, (const uint8_t[]){0x00, 0x00, 0x04, 0xcf}, sizeof((const uint8_t[]){0x00, 0x00, 0x04, 0xcf}), 0},
    {0x12, (const uint8_t[]){0x00}, sizeof((const uint8_t[]){0x00}), 0},
    {0x35, (const uint8_t[]){0x00}, sizeof((const uint8_t[]){0x00}), 0},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};

static esp_err_t rm69a10_check_id(esp_lcd_panel_io_handle_t io)
{
    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(io, 0xa1, &id, sizeof(id)),
                        TAG, "read id failed");
    ESP_RETURN_ON_FALSE(id == 0x01, ESP_FAIL, TAG, "unexpected id: 0x%02x", id);
    return ESP_OK;
}

static esp_err_t rm69a10_send_init_cmds(esp_lcd_panel_io_handle_t io)
{
    for (size_t i = 0; i < sizeof(s_rm69a10_init_cmds) / sizeof(s_rm69a10_init_cmds[0]); i++) {
        const rm69a10_lcd_init_cmd_t *cmd = &s_rm69a10_init_cmds[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, cmd->cmd, cmd->data, cmd->data_len),
                            TAG, "send init command 0x%02x failed", cmd->cmd);
        if (cmd->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }
    return ESP_OK;
}

static esp_err_t panel_rm69a10_reset(esp_lcd_panel_t *panel)
{
    rm69a10_panel_t *rm69a10 = __containerof(panel, rm69a10_panel_t, base);
    return esp_lcd_panel_reset(rm69a10->dpi_panel);
}

static esp_err_t panel_rm69a10_init(esp_lcd_panel_t *panel)
{
    rm69a10_panel_t *rm69a10 = __containerof(panel, rm69a10_panel_t, base);
    ESP_RETURN_ON_ERROR(rm69a10_check_id(rm69a10->io), TAG, "check id failed");
    ESP_RETURN_ON_ERROR(rm69a10_send_init_cmds(rm69a10->io), TAG, "send init commands failed");
    ESP_RETURN_ON_ERROR(panel_rm69a10_set_brightness(panel, 0xff),
                        TAG, "set default brightness failed");
    return esp_lcd_panel_init(rm69a10->dpi_panel);
}

static esp_err_t panel_rm69a10_del(esp_lcd_panel_t *panel)
{
    rm69a10_panel_t *rm69a10 = __containerof(panel, rm69a10_panel_t, base);
    esp_err_t ret = esp_lcd_panel_del(rm69a10->dpi_panel);
    free(rm69a10);
    return ret;
}

static esp_err_t panel_rm69a10_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data)
{
    rm69a10_panel_t *rm69a10 = __containerof(panel, rm69a10_panel_t, base);
    return esp_lcd_panel_draw_bitmap(rm69a10->dpi_panel, x_start, y_start, x_end, y_end, color_data);
}

static esp_err_t panel_rm69a10_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    return (!mirror_x && !mirror_y) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t panel_rm69a10_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    return swap_axes ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

static esp_err_t panel_rm69a10_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    rm69a10_panel_t *rm69a10 = __containerof(panel, rm69a10_panel_t, base);
    return esp_lcd_panel_set_gap(rm69a10->dpi_panel, x_gap, y_gap);
}

static esp_err_t panel_rm69a10_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    rm69a10_panel_t *rm69a10 = __containerof(panel, rm69a10_panel_t, base);
    return esp_lcd_panel_io_tx_param(rm69a10->io,
                                     invert_color_data ? RM69A10_CMD_INVON : RM69A10_CMD_INVOFF,
                                     NULL, 0);
}

static esp_err_t panel_rm69a10_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    rm69a10_panel_t *rm69a10 = __containerof(panel, rm69a10_panel_t, base);
    return esp_lcd_panel_io_tx_param(rm69a10->io,
                                     on_off ? RM69A10_CMD_DISPON : RM69A10_CMD_DISPOFF,
                                     NULL, 0);
}

static esp_err_t panel_rm69a10_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    rm69a10_panel_t *rm69a10 = __containerof(panel, rm69a10_panel_t, base);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(rm69a10->io,
                                                  sleep ? RM69A10_CMD_SLPIN : RM69A10_CMD_SLPOUT,
                                                  NULL, 0),
                        TAG, "set sleep mode failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t panel_rm69a10_set_brightness(esp_lcd_panel_t *panel, int brightness)
{
    rm69a10_panel_t *rm69a10 = __containerof(panel, rm69a10_panel_t, base);
    ESP_RETURN_ON_FALSE(brightness >= 0 && brightness <= 0xff,
                        ESP_ERR_INVALID_ARG, TAG, "brightness out of range");
    uint8_t brightness_value = (uint8_t)brightness;
    return esp_lcd_panel_io_tx_param(rm69a10->io, RM69A10_CMD_WRDISBV,
                                     &brightness_value, sizeof(brightness_value));
}

esp_err_t esp_lcd_new_panel_rm69a10(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    rm69a10_panel_t *rm69a10 = NULL;
    esp_lcd_panel_handle_t dpi_panel = NULL;

    ESP_RETURN_ON_FALSE(io != NULL && panel_dev_config != NULL && ret_panel != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    const rm69a10_vendor_config_t *vendor = panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor != NULL && vendor->dsi_bus != NULL && vendor->dpi_config != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid vendor config");

    rm69a10 = calloc(1, sizeof(rm69a10_panel_t));
    ESP_RETURN_ON_FALSE(rm69a10 != NULL, ESP_ERR_NO_MEM, TAG, "no memory for RM69A10 panel");

    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor->dsi_bus, vendor->dpi_config, &dpi_panel),
                      err, TAG, "create DPI panel failed");

    rm69a10->dpi_panel = dpi_panel;
    rm69a10->io = io;
    rm69a10->base.reset = panel_rm69a10_reset;
    rm69a10->base.init = panel_rm69a10_init;
    rm69a10->base.del = panel_rm69a10_del;
    rm69a10->base.draw_bitmap = panel_rm69a10_draw_bitmap;
    rm69a10->base.mirror = panel_rm69a10_mirror;
    rm69a10->base.swap_xy = panel_rm69a10_swap_xy;
    rm69a10->base.set_gap = panel_rm69a10_set_gap;
    rm69a10->base.invert_color = panel_rm69a10_invert_color;
    rm69a10->base.disp_on_off = panel_rm69a10_disp_on_off;
    rm69a10->base.disp_sleep = panel_rm69a10_disp_sleep;
    rm69a10->base.set_brightness = panel_rm69a10_set_brightness;

    *ret_panel = &rm69a10->base;
    return ESP_OK;

err:
    if (dpi_panel != NULL) {
        esp_lcd_panel_del(dpi_panel);
    }
    free(rm69a10);
    return ret;
}
