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
#include "esp_lcd_fl7707n.h"

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save surrent value of LCD_CMD_COLMOD register
    const fl7707n_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int reset_level: 1;
    } flags;
    // To save the original functions of MIPI DPI panel
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} fl7707n_panel_t;

static const char *TAG = "fl7707n";

static esp_err_t panel_fl7707n_del(esp_lcd_panel_t *panel);
static esp_err_t panel_fl7707n_init(esp_lcd_panel_t *panel);
static esp_err_t panel_fl7707n_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_fl7707n_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_fl7707n_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

esp_err_t esp_lcd_new_panel_fl7707n(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    // ESP_LOGI(TAG, "version: %d.%d.%d", ESP_LCD_FL7707N_VER_MAJOR, ESP_LCD_FL7707N_VER_MINOR,
    //          ESP_LCD_FL7707N_VER_PATCH);
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    fl7707n_vendor_config_t *vendor_config = (fl7707n_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->mipi_config.dpi_config && vendor_config->mipi_config.dsi_bus, ESP_ERR_INVALID_ARG, TAG,
                        "invalid vendor config");

    esp_err_t ret = ESP_OK;
    fl7707n_panel_t *fl7707n = (fl7707n_panel_t *)calloc(1, sizeof(fl7707n_panel_t));
    ESP_RETURN_ON_FALSE(fl7707n, ESP_ERR_NO_MEM, TAG, "no mem for fl7707n panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        fl7707n->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        fl7707n->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        fl7707n->colmod_val = 0x55;
        break;
    case 18: // RGB666
        fl7707n->colmod_val = 0x66;
        break;
    case 24: // RGB888
        fl7707n->colmod_val = 0x77;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    fl7707n->io = io;
    fl7707n->init_cmds = vendor_config->init_cmds;
    fl7707n->init_cmds_size = vendor_config->init_cmds_size;
    fl7707n->reset_gpio_num = panel_dev_config->reset_gpio_num;
    fl7707n->flags.reset_level = panel_dev_config->flags.reset_active_high;

    // Create MIPI DPI panel
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus, vendor_config->mipi_config.dpi_config, &panel_handle), err, TAG,
                      "create MIPI DPI panel failed");
    ESP_LOGD(TAG, "new MIPI DPI panel @%p", panel_handle);

    // Save the original functions of MIPI DPI panel
    fl7707n->del = panel_handle->del;
    fl7707n->init = panel_handle->init;
    // Overwrite the functions of MIPI DPI panel
    panel_handle->del = panel_fl7707n_del;
    panel_handle->init = panel_fl7707n_init;
    panel_handle->reset = panel_fl7707n_reset;
    panel_handle->invert_color = panel_fl7707n_invert_color;
    panel_handle->disp_on_off = panel_fl7707n_disp_on_off;
    panel_handle->user_data = fl7707n;
    *ret_panel = panel_handle;
    ESP_LOGD(TAG, "new fl7707n panel @%p", fl7707n);

    return ESP_OK;

err:
    if (fl7707n) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(fl7707n);
    }
    return ret;
}

static const fl7707n_lcd_init_cmd_t vendor_specific_init_default[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xB9, (uint8_t[]){0xF1, 0x12, 0x87}, 3, 0},
    {0xB2, (uint8_t[]){0xB4, 0x03, 0x70}, 3, 0},
    {0xB3, (uint8_t[]){0x10, 0x10, 0x28, 0x28, 0x03, 0xFF, 0x00, 0x00, 0x00, 0x00}, 10, 0},
    {0xB4, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x0A, 0x0A}, 2, 0},
    {0xB6, (uint8_t[]){0x8D, 0x8D}, 2, 0},
    {0xB8, (uint8_t[]){0x26, 0x22, 0xF0, 0x13}, 4, 0},
    {0xBA, (uint8_t[]){
        0x31, 0x81, 0x05, 0xF9, 0x0E, 0x0E, 0x20, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x44, 0x25, 0x00, 0x91, 0x0A, 0x00,
        0x00, 0x01, 0x4F, 0x01, 0x00, 0x00, 0x37
    }, 27, 0},
    {0xBC, (uint8_t[]){0x47}, 1, 0},
    {0xBF, (uint8_t[]){0x02, 0x10, 0x00, 0x80, 0x04}, 5, 0},
    {0xC0, (uint8_t[]){0x73, 0x73, 0x50, 0x50, 0x00, 0x00, 0x12, 0x73, 0x00}, 9, 0},
    {0xC1, (uint8_t[]){
        0x36, 0x00, 0x32, 0x32, 0x77, 0xE1, 0x77, 0x77, 0xCC, 0xCC,
        0xFF, 0xFF, 0x11, 0x11, 0x00, 0x00, 0x32
    }, 17, 0},
    {0xC7, (uint8_t[]){
        0x10, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0xED, 0xC5,
        0x00, 0xA5
    }, 12, 0},
    {0xC8, (uint8_t[]){0x10, 0x40, 0x1E, 0x03}, 4, 0},
    {0xCC, (uint8_t[]){0x0B}, 1, 0},
    {0xE0, (uint8_t[]){
        0x00, 0x0A, 0x0F, 0x2A, 0x33, 0x3F, 0x44, 0x39, 0x06, 0x0C,
        0x0E, 0x14, 0x15, 0x13, 0x15, 0x10, 0x18,
        0x00, 0x0A, 0x0F, 0x2A, 0x33, 0x3F, 0x44, 0x39, 0x06, 0x0C,
        0x0E, 0x14, 0x15, 0x13, 0x15, 0x10, 0x18
    }, 34, 0},
    {0xE1, (uint8_t[]){0x11, 0x11, 0x91, 0x00, 0x00, 0x00, 0x00}, 7, 0},
    {0xE3, (uint8_t[]){
        0x07, 0x07, 0x0B, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0x04, 0xC0, 0x10
    }, 14, 0},
    {0xE9, (uint8_t[]){
        0xC8, 0x10, 0x0A, 0x00, 0x00, 0x80, 0x81, 0x12, 0x31, 0x23,
        0x4F, 0x86, 0xA0, 0x00, 0x47, 0x08, 0x00, 0x00, 0x0C, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x98, 0x02,
        0x8B, 0xAF, 0x46, 0x02, 0x88, 0x88, 0x88, 0x88, 0x88, 0x98,
        0x13, 0x8B, 0xAF, 0x57, 0x13, 0x88, 0x88, 0x88, 0x88, 0x88,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    }, 64, 0},
    {0xEA, (uint8_t[]){
        0x97, 0x0C, 0x09, 0x09, 0x09, 0x78, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x9F, 0x31, 0x8B, 0xA8, 0x31, 0x75, 0x88, 0x88,
        0x88, 0x88, 0x88, 0x9F, 0x20, 0x8B, 0xA8, 0x20, 0x64, 0x88,
        0x88, 0x88, 0x88, 0x88, 0x23, 0x00, 0x00, 0x02, 0x71, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x40, 0x80, 0x81, 0x00, 0x00, 0x00, 0x00
    }, 59, 0},
    {0xEF, (uint8_t[]){0xFF, 0xFF, 0x01}, 3, 0},
    {LCD_CMD_SLPOUT, NULL, 0, 250},
    {LCD_CMD_DISPON, NULL, 0, 50},
};

static esp_err_t panel_fl7707n_del(esp_lcd_panel_t *panel)
{
    fl7707n_panel_t *fl7707n = (fl7707n_panel_t *)panel->user_data;

    if (fl7707n->reset_gpio_num >= 0) {
        gpio_reset_pin(fl7707n->reset_gpio_num);
    }
    // Delete MIPI DPI panel
    fl7707n->del(panel);
    ESP_LOGD(TAG, "del fl7707n panel @%p", fl7707n);
    free(fl7707n);

    return ESP_OK;
}

static esp_err_t panel_fl7707n_init(esp_lcd_panel_t *panel)
{
    fl7707n_panel_t *fl7707n = (fl7707n_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = fl7707n->io;
    const fl7707n_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_cmd_overwritten = false;

    // vendor specific initialization, it can be different between manufacturers
    // should consult the LCD supplier for initialization sequence code
    if (fl7707n->init_cmds) {
        init_cmds = fl7707n->init_cmds;
        init_cmds_size = fl7707n->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(fl7707n_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal
        if (init_cmds[i].data_bytes > 0) {
            switch (init_cmds[i].cmd) {
            case LCD_CMD_MADCTL:
                is_cmd_overwritten = true;
                fl7707n->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            case LCD_CMD_COLMOD:
                is_cmd_overwritten = true;
                fl7707n->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
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

    ESP_RETURN_ON_ERROR(fl7707n->init(panel), TAG, "init MIPI DPI panel failed");

    return ESP_OK;
}

static esp_err_t panel_fl7707n_reset(esp_lcd_panel_t *panel)
{
    fl7707n_panel_t *fl7707n = (fl7707n_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = fl7707n->io;

    // Perform hardware reset
    if (fl7707n->reset_gpio_num >= 0) {
        gpio_set_level(fl7707n->reset_gpio_num, fl7707n->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(fl7707n->reset_gpio_num, !fl7707n->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else if (io) { // Perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_fl7707n_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    fl7707n_panel_t *fl7707n = (fl7707n_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = fl7707n->io;
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

static esp_err_t panel_fl7707n_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    fl7707n_panel_t *fl7707n = (fl7707n_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = fl7707n->io;
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