#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_board_manager_includes.h"
#include "gen_board_device_custom.h"

static const char *TAG = "ANYBOARD_SETUP_DEVICE";

#define USB_UVC_DEV_NUM             1
#define USB_HOST_TASK_PRIORITY      5
#define USB_HOST_TASK_STACK_SIZE    4096
#define USB_UVC_TASK_PRIORITY       configMAX_PRIORITIES - 2
#define USB_UVC_TASK_STACK_SIZE     4096

typedef struct {
    dev_camera_handle_t handle;
} custom_usb_camera_handle_t;

static int usb_camera_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid camera handle");

#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    const esp_video_init_usb_uvc_config_t usb_uvc_config = {
        .uvc = {
            .uvc_dev_num = USB_UVC_DEV_NUM,
            .task_stack = USB_UVC_TASK_STACK_SIZE,
            .task_priority = USB_UVC_TASK_PRIORITY,
            .task_affinity = 0,
        },
        .usb = {
            .init_usb_host_lib = true,
            .task_stack = USB_HOST_TASK_STACK_SIZE,
            .task_priority = USB_HOST_TASK_PRIORITY,
            .task_affinity = 0,
        },
    };
    const esp_video_init_config_t video_config = {
        .usb_uvc = &usb_uvc_config,
    };

    esp_err_t ret = esp_video_init(&video_config);
    if (ret != ESP_OK) {
        return ret;
    }

    custom_usb_camera_handle_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        (void)esp_video_deinit();
        return ESP_ERR_NO_MEM;
    }

    handle->handle.dev_path = ESP_VIDEO_USB_UVC_NAME(0);
    handle->handle.meta_path = "";
    *device_handle = &handle->handle;
    ESP_LOGI(TAG, "USB UVC camera initialized, dev_path: %s", handle->handle.dev_path);
    return ESP_OK;
#else
    ESP_LOGE(TAG, "USB UVC camera is disabled. Enable CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static int usb_camera_deinit(void *device_handle)
{
#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    free(device_handle);
    ESP_RETURN_ON_ERROR(esp_video_deinit(), TAG, "failed to deinit USB UVC camera");
    ESP_LOGI(TAG, "USB UVC camera deinitialized");
    return ESP_OK;
#else
    (void)device_handle;
    return ESP_OK;
#endif
}

CUSTOM_DEVICE_IMPLEMENT(camera, usb_camera_init, usb_camera_deinit);

/*
 * The AnyBoard LCD uses a cropped ST7789 glass. Without a panel gap, the
 * top rows are addressed outside the visible window after landscape rotation.
 */
#define ANYBOARD_LCD_OFFSET_X 0
#define ANYBOARD_LCD_OFFSET_Y 16

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} st7789_lcd_init_cmd_t;

static const st7789_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0xB2, (uint8_t []){0x0C, 0x0C, 0x00, 0x33, 0x33}, 5, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x36, (uint8_t []){0x00}, 1, 0},
    {0x3A, (uint8_t []){0x05}, 1, 0},
    {0xB7, (uint8_t []){0x35}, 1, 0},
    {0xBB, (uint8_t []){0x2D}, 1, 0},
    {0xC0, (uint8_t []){0x2C}, 1, 0},
    {0xC2, (uint8_t []){0x01}, 1, 0},
    {0xC3, (uint8_t []){0x15}, 1, 0},
    {0xC4, (uint8_t []){0x20}, 1, 0},
    {0xC6, (uint8_t []){0x0F}, 1, 0},
    {0xD0, (uint8_t []){0xA4, 0xA1}, 2, 0},
    {0xD6, (uint8_t []){0xA1}, 1, 0},
    {0xE0, (uint8_t []){0x70, 0x05, 0x0A, 0x0B, 0x0A, 0x27, 0x2F, 0x44, 0x47, 0x37, 0x14, 0x14, 0x29, 0x2F}, 14, 0},
    {0xE1, (uint8_t []){0x70, 0x07, 0x0C, 0x08, 0x08, 0x04, 0x2F, 0x33, 0x46, 0x18, 0x15, 0x15, 0x2B, 0x2D}, 14, 0},
    {0x21, (uint8_t []){0x00}, 1, 0},
    {0x29, (uint8_t []){0x00}, 1, 0},
    {0x2C, (uint8_t []){0x00}, 1, 0},
};

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = esp_lcd_new_panel_st7789(io, panel_dev_config, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_set_gap(*ret_panel, ANYBOARD_LCD_OFFSET_X, ANYBOARD_LCD_OFFSET_Y);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_panel_set_gap failed: %s", esp_err_to_name(ret));
    }

    for (size_t i = 0; i < sizeof(vendor_specific_init) / sizeof(vendor_specific_init[0]); i++) {
        ret = esp_lcd_panel_io_tx_param(io, vendor_specific_init[i].cmd,
                                        vendor_specific_init[i].data,
                                        vendor_specific_init[i].data_bytes);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send init cmd 0x%02x: %s", vendor_specific_init[i].cmd, esp_err_to_name(ret));
            return ret;
        }
        if (vendor_specific_init[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(vendor_specific_init[i].delay_ms));
        }
    }

    esp_lcd_panel_reset(*ret_panel);
    esp_lcd_panel_init(*ret_panel);

    return ESP_OK;
}
