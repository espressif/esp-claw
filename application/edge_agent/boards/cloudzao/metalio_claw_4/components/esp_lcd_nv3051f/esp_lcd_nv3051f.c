#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_nv3051f.h"

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save current value of LCD_CMD_COLMOD register
    const nv3051f_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int reset_level: 1;
    } flags;
    // To save the original functions of MIPI DPI panel
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} nv3051f_panel_t;

static const char *TAG = "nv3051f";

static esp_err_t panel_nv3051f_del(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3051f_init(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3051f_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3051f_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_nv3051f_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

esp_err_t esp_lcd_new_panel_nv3051f(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    nv3051f_vendor_config_t *vendor_config = (nv3051f_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->mipi_config.dpi_config && vendor_config->mipi_config.dsi_bus, ESP_ERR_INVALID_ARG, TAG,
                        "invalid vendor config");

    esp_err_t ret = ESP_OK;
    nv3051f_panel_t *nv3051f = (nv3051f_panel_t *)calloc(1, sizeof(nv3051f_panel_t));
    ESP_RETURN_ON_FALSE(nv3051f, ESP_ERR_NO_MEM, TAG, "no mem for nv3051f panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        nv3051f->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        nv3051f->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        nv3051f->colmod_val = 0x55;
        break;
    case 18: // RGB666
        nv3051f->colmod_val = 0x66;
        break;
    case 24: // RGB888
        nv3051f->colmod_val = 0x77;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    nv3051f->io = io;
    nv3051f->init_cmds = vendor_config->init_cmds;
    nv3051f->init_cmds_size = vendor_config->init_cmds_size;
    nv3051f->reset_gpio_num = panel_dev_config->reset_gpio_num;
    nv3051f->flags.reset_level = panel_dev_config->flags.reset_active_high;

    // Create MIPI DPI panel
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus, vendor_config->mipi_config.dpi_config, &panel_handle), err, TAG,
                      "create MIPI DPI panel failed");
    ESP_LOGD(TAG, "new MIPI DPI panel @%p", panel_handle);

    // Save the original functions of MIPI DPI panel
    nv3051f->del = panel_handle->del;
    nv3051f->init = panel_handle->init;
    // Overwrite the functions of MIPI DPI panel
    panel_handle->del = panel_nv3051f_del;
    panel_handle->init = panel_nv3051f_init;
    panel_handle->reset = panel_nv3051f_reset;
    panel_handle->invert_color = panel_nv3051f_invert_color;
    panel_handle->disp_on_off = panel_nv3051f_disp_on_off;
    panel_handle->user_data = nv3051f;
    *ret_panel = panel_handle;
    ESP_LOGD(TAG, "new nv3051f panel @%p", nv3051f);

    return ESP_OK;

err:
    if (nv3051f) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(nv3051f);
    }
    return ret;
}

// NV3051F vendor specific initialization sequence
// Source: TRULY 3.95" (HE396-040T2BZZ) + NV3051F (Positive scan) GAMMA2.2 / 20250708
static const nv3051f_lcd_init_cmd_t vendor_specific_init_default[] = {
    //  {cmd, { data }, data_size, delay_ms}

    // ---------- Page 1 ----------
    {0xFF, (uint8_t[]){0x30}, 1, 0},
    {0xFF, (uint8_t[]){0x52}, 1, 0},
    {0xFF, (uint8_t[]){0x01}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0x20, (uint8_t[]){0x90}, 1, 0}, // MIPI 2 LANE
    {0x28, (uint8_t[]){0x5F}, 1, 0},
    {0x29, (uint8_t[]){0x02}, 1, 0},
    {0x2A, (uint8_t[]){0xCF}, 1, 0},
    {0x30, (uint8_t[]){0x58}, 1, 0},
    {0x37, (uint8_t[]){0x9C}, 1, 0},
    {0x38, (uint8_t[]){0xA7}, 1, 0},
    {0x39, (uint8_t[]){0x43}, 1, 0}, // VCOM
    {0x44, (uint8_t[]){0x00}, 1, 0},
    {0x49, (uint8_t[]){0x3C}, 1, 0},
    {0x59, (uint8_t[]){0xFE}, 1, 0},
    {0x5C, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x20}, 1, 0}, // 2 power
    {0x91, (uint8_t[]){0x77}, 1, 0},
    {0x92, (uint8_t[]){0x77}, 1, 0},
    {0xA0, (uint8_t[]){0x55}, 1, 0},
    {0xA1, (uint8_t[]){0x50}, 1, 0},
    {0xA3, (uint8_t[]){0x58}, 1, 0},
    {0xA4, (uint8_t[]){0x9C}, 1, 0},
    {0xA7, (uint8_t[]){0x02}, 1, 0},
    {0xA8, (uint8_t[]){0x01}, 1, 0},
    {0xA9, (uint8_t[]){0x21}, 1, 0},
    {0xAA, (uint8_t[]){0xFC}, 1, 0},
    {0xAB, (uint8_t[]){0x28}, 1, 0},
    {0xAC, (uint8_t[]){0x06}, 1, 0},
    {0xAD, (uint8_t[]){0x06}, 1, 0},
    {0xAE, (uint8_t[]){0x06}, 1, 0},
    {0xAF, (uint8_t[]){0x03}, 1, 0},
    {0xB0, (uint8_t[]){0x08}, 1, 0},
    {0xB1, (uint8_t[]){0x26}, 1, 0},
    {0xB2, (uint8_t[]){0x28}, 1, 0},
    {0xB3, (uint8_t[]){0x28}, 1, 0},
    {0xB4, (uint8_t[]){0x03}, 1, 0},
    {0xB5, (uint8_t[]){0x08}, 1, 0},
    {0xB6, (uint8_t[]){0x26}, 1, 0},
    {0xB7, (uint8_t[]){0x08}, 1, 0},
    {0xB8, (uint8_t[]){0x26}, 1, 0},
    {0xC0, (uint8_t[]){0x00}, 1, 0},
    {0xC1, (uint8_t[]){0x00}, 1, 0},
    {0xC2, (uint8_t[]){0x00}, 1, 0},
    {0xC3, (uint8_t[]){0x0F}, 1, 0},

    // ---------- Page 2 ----------
    {0xFF, (uint8_t[]){0x30}, 1, 0},
    {0xFF, (uint8_t[]){0x52}, 1, 0},
    {0xFF, (uint8_t[]){0x02}, 1, 0},
    {0xB5, (uint8_t[]){0x37}, 1, 0},
    {0xB1, (uint8_t[]){0x0B}, 1, 0},
    {0xB2, (uint8_t[]){0x0A}, 1, 0},
    {0xB3, (uint8_t[]){0x2F}, 1, 0},
    {0xB4, (uint8_t[]){0x30}, 1, 0},
    {0xB0, (uint8_t[]){0x02}, 1, 0},
    {0xB6, (uint8_t[]){0x15}, 1, 0},
    {0xB7, (uint8_t[]){0x37}, 1, 0},
    {0xB8, (uint8_t[]){0x0B}, 1, 0},
    {0xB9, (uint8_t[]){0x02}, 1, 0},
    {0xBA, (uint8_t[]){0x0F}, 1, 0},
    {0xBB, (uint8_t[]){0x0F}, 1, 0},
    {0xBC, (uint8_t[]){0x10}, 1, 0},
    {0xBD, (uint8_t[]){0x12}, 1, 0},
    {0xBE, (uint8_t[]){0x18}, 1, 0},
    {0xBF, (uint8_t[]){0x0F}, 1, 0},
    {0xC0, (uint8_t[]){0x17}, 1, 0},
    {0xC1, (uint8_t[]){0x05}, 1, 0},
    {0xD5, (uint8_t[]){0x32}, 1, 0},
    {0xD1, (uint8_t[]){0x07}, 1, 0},
    {0xD2, (uint8_t[]){0x06}, 1, 0},
    {0xD3, (uint8_t[]){0x2F}, 1, 0},
    {0xD4, (uint8_t[]){0x30}, 1, 0},
    {0xD0, (uint8_t[]){0x05}, 1, 0},
    {0xD6, (uint8_t[]){0x13}, 1, 0},
    {0xD7, (uint8_t[]){0x37}, 1, 0},
    {0xD8, (uint8_t[]){0x0D}, 1, 0},
    {0xD9, (uint8_t[]){0x04}, 1, 0},
    {0xDA, (uint8_t[]){0x11}, 1, 0},
    {0xDB, (uint8_t[]){0x0F}, 1, 0},
    {0xDC, (uint8_t[]){0x10}, 1, 0},
    {0xDD, (uint8_t[]){0x12}, 1, 0},
    {0xDE, (uint8_t[]){0x1A}, 1, 0},
    {0xDF, (uint8_t[]){0x11}, 1, 0},
    {0xE0, (uint8_t[]){0x19}, 1, 0},
    {0xE1, (uint8_t[]){0x07}, 1, 0},

    // ---------- Page 3 ----------
    {0xFF, (uint8_t[]){0x30}, 1, 0},
    {0xFF, (uint8_t[]){0x52}, 1, 0},
    {0xFF, (uint8_t[]){0x03}, 1, 0},
    {0x08, (uint8_t[]){0x8A}, 1, 0},
    {0x09, (uint8_t[]){0x8B}, 1, 0},
    {0x0A, (uint8_t[]){0x88}, 1, 0},
    {0x0B, (uint8_t[]){0x89}, 1, 0},
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0x31, (uint8_t[]){0x00}, 1, 0},
    {0x32, (uint8_t[]){0x00}, 1, 0},
    {0x33, (uint8_t[]){0x00}, 1, 0},
    {0x34, (uint8_t[]){0xA1}, 1, 0},
    {0x35, (uint8_t[]){0x07}, 1, 0},
    {0x36, (uint8_t[]){0x60}, 1, 0},
    {0x37, (uint8_t[]){0x03}, 1, 0},
    {0x40, (uint8_t[]){0x86}, 1, 0},
    {0x41, (uint8_t[]){0x87}, 1, 0},
    {0x42, (uint8_t[]){0x84}, 1, 0},
    {0x43, (uint8_t[]){0x85}, 1, 0},
    {0x44, (uint8_t[]){0x22}, 1, 0},
    {0x45, (uint8_t[]){0xCE}, 1, 0},
    {0x46, (uint8_t[]){0xCD}, 1, 0},
    {0x47, (uint8_t[]){0x22}, 1, 0},
    {0x48, (uint8_t[]){0xD0}, 1, 0},
    {0x49, (uint8_t[]){0xCF}, 1, 0},
    {0x50, (uint8_t[]){0x82}, 1, 0},
    {0x51, (uint8_t[]){0x83}, 1, 0},
    {0x52, (uint8_t[]){0x80}, 1, 0},
    {0x53, (uint8_t[]){0x81}, 1, 0},
    {0x54, (uint8_t[]){0x22}, 1, 0},
    {0x55, (uint8_t[]){0xD2}, 1, 0},
    {0x56, (uint8_t[]){0xD1}, 1, 0},
    {0x57, (uint8_t[]){0x22}, 1, 0},
    {0x58, (uint8_t[]){0xD4}, 1, 0},
    {0x59, (uint8_t[]){0xD3}, 1, 0},
    {0x7E, (uint8_t[]){0x3C}, 1, 0},
    {0x7F, (uint8_t[]){0xC0}, 1, 0},
    {0x80, (uint8_t[]){0x0C}, 1, 0},
    {0x81, (uint8_t[]){0x0D}, 1, 0},
    {0x82, (uint8_t[]){0x0F}, 1, 0},
    {0x83, (uint8_t[]){0x0F}, 1, 0},
    {0x84, (uint8_t[]){0x0E}, 1, 0},
    {0x85, (uint8_t[]){0x06}, 1, 0},
    {0x86, (uint8_t[]){0x07}, 1, 0},
    {0x87, (uint8_t[]){0x04}, 1, 0},
    {0x88, (uint8_t[]){0x05}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x01}, 1, 0},
    {0x8B, (uint8_t[]){0x0F}, 1, 0},
    {0x96, (uint8_t[]){0x0C}, 1, 0},
    {0x97, (uint8_t[]){0x0D}, 1, 0},
    {0x98, (uint8_t[]){0x0F}, 1, 0},
    {0x99, (uint8_t[]){0x0F}, 1, 0},
    {0x9A, (uint8_t[]){0x0E}, 1, 0},
    {0x9B, (uint8_t[]){0x06}, 1, 0},
    {0x9C, (uint8_t[]){0x07}, 1, 0},
    {0x9D, (uint8_t[]){0x04}, 1, 0},
    {0x9E, (uint8_t[]){0x05}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x01}, 1, 0},
    {0xA1, (uint8_t[]){0x0F}, 1, 0},

    // ---------- Page 0 ----------
    {0xFF, (uint8_t[]){0x30}, 1, 0},
    {0xFF, (uint8_t[]){0x52}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0x02}, 1, 0},

    // Exit sleep then display on
    {LCD_CMD_SLPOUT, NULL, 0, 200},
    {LCD_CMD_DISPON, NULL, 0, 100},
};

esp_err_t esp_lcd_nv3051f_replay_vendor_init(esp_lcd_panel_io_handle_t io)
{
    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_ARG, TAG, "invalid panel IO");
    const uint16_t n = sizeof(vendor_specific_init_default) / sizeof(nv3051f_lcd_init_cmd_t);
    for (uint16_t i = 0; i < n; i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io,
                                                      vendor_specific_init_default[i].cmd,
                                                      vendor_specific_init_default[i].data,
                                                      vendor_specific_init_default[i].data_bytes),
                            TAG, "replay vendor cmd 0x%02X failed", vendor_specific_init_default[i].cmd);
        if (vendor_specific_init_default[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(vendor_specific_init_default[i].delay_ms));
        }
    }
    return ESP_OK;
}

static esp_err_t panel_nv3051f_del(esp_lcd_panel_t *panel)
{
    nv3051f_panel_t *nv3051f = (nv3051f_panel_t *)panel->user_data;

    if (nv3051f->reset_gpio_num >= 0) {
        gpio_reset_pin(nv3051f->reset_gpio_num);
    }
    // Delete MIPI DPI panel
    nv3051f->del(panel);
    ESP_LOGD(TAG, "del nv3051f panel @%p", nv3051f);
    free(nv3051f);

    return ESP_OK;
}

static esp_err_t panel_nv3051f_init(esp_lcd_panel_t *panel)
{
    nv3051f_panel_t *nv3051f = (nv3051f_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = nv3051f->io;
    const nv3051f_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_cmd_overwritten = false;

    // vendor specific initialization, it can be different between manufacturers
    // should consult the LCD supplier for initialization sequence code
    if (nv3051f->init_cmds) {
        init_cmds = nv3051f->init_cmds;
        init_cmds_size = nv3051f->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(nv3051f_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal
        if (init_cmds[i].data_bytes > 0) {
            switch (init_cmds[i].cmd) {
            case LCD_CMD_MADCTL:
                is_cmd_overwritten = true;
                nv3051f->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            case LCD_CMD_COLMOD:
                is_cmd_overwritten = true;
                nv3051f->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            default:
                is_cmd_overwritten = false;
                break;
            }

            if (is_cmd_overwritten) {
                is_cmd_overwritten = false;
                ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence",
                         init_cmds[i].cmd);
            }
        }

        // Send command
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }
    ESP_LOGD(TAG, "send init commands success");

    ESP_RETURN_ON_ERROR(nv3051f->init(panel), TAG, "init MIPI DPI panel failed");

    return ESP_OK;
}

static esp_err_t panel_nv3051f_reset(esp_lcd_panel_t *panel)
{
    nv3051f_panel_t *nv3051f = (nv3051f_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = nv3051f->io;

    // Perform hardware reset
    if (nv3051f->reset_gpio_num >= 0) {
        gpio_set_level(nv3051f->reset_gpio_num, nv3051f->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(nv3051f->reset_gpio_num, !nv3051f->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else if (io) { // Perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_nv3051f_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    nv3051f_panel_t *nv3051f = (nv3051f_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = nv3051f->io;
    uint8_t command = 0;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");

    return ESP_OK;
}

static esp_err_t panel_nv3051f_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    nv3051f_panel_t *nv3051f = (nv3051f_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = nv3051f->io;
    int command = 0;

    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
#endif
