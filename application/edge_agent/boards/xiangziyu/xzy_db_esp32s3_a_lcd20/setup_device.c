/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_board_manager_includes.h"
#include "gen_board_device_custom.h"
#include "uac_codec.h"
#include "esp_lcd_panel_st7789.h"


static const char *TAG = "setup_device";

typedef enum {
    USB_DEVICE_KIND_CAMERA,
    USB_DEVICE_KIND_UAC_SPK,
    USB_DEVICE_KIND_UAC_MIC,
} usb_device_kind_t;

typedef struct {
    dev_camera_handle_t handle;
    usb_device_kind_t kind;
} custom_usb_device_handle_t;

typedef struct {
    uint8_t addr;
    uint8_t iface_num;
    uac_host_driver_event_t event;
} uac_driver_event_msg_t;

#define USB_HOST_TASK_PRIORITY      5
#define USB_HOST_TASK_STACK_SIZE    4096
#define UAC_TASK_PRIORITY           configMAX_PRIORITIES - 2
#define UAC_TASK_STACK_SIZE         4096
#define UAC_EVENT_QUEUE_LEN         8
#define UAC_CONNECT_TIMEOUT_MS      1500
#define USB_UVC_DEV_NUM             1
#define USB_UVC_TASK_PRIORITY       configMAX_PRIORITIES - 2
#define USB_UVC_TASK_STACK_SIZE     4096

static SemaphoreHandle_t s_usb_lock;
static TaskHandle_t s_usb_task_handle;
static int s_usb_ref_count;
static bool s_usb_stop_requested;

static QueueHandle_t s_uac_event_queue;
static TaskHandle_t s_uac_task_handle;
static bool s_uac_installed;
static int s_uac_ref_count;

static uac_driver_event_msg_t s_spk_dev_info;
static uac_driver_event_msg_t s_mic_dev_info;
static bool s_spk_dev_found;
static bool s_mic_dev_found;

static void usb_lib_task(void *arg)
{
    while (true) {
        uint32_t event_flags = 0;

        if (usb_host_lib_handle_events(portMAX_DELAY, &event_flags) != ESP_OK) {
            ESP_LOGE(TAG, "USB host event handling failed");
            break;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            (void)usb_host_device_free_all();
            if (s_usb_stop_requested) {
                break;
            }
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB host has freed all devices");
            if (s_usb_stop_requested) {
                break;
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    esp_err_t ret = usb_host_uninstall();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to uninstall USB host: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "USB host uninstalled");
    }
    s_usb_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t shared_usb_host_acquire(void)
{
    esp_err_t ret = ESP_OK;

    if (s_usb_lock == NULL) {
        s_usb_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_usb_lock != NULL, ESP_ERR_NO_MEM, TAG, "failed to create USB lock");
    }

    xSemaphoreTake(s_usb_lock, portMAX_DELAY);
    if (s_usb_ref_count == 0) {
        s_usb_stop_requested = false;

        if (s_usb_task_handle == NULL) {
            const usb_host_config_t host_config = {
                .skip_phy_setup = false,
                .intr_flags = ESP_INTR_FLAG_LOWMED,
            };

            ret = usb_host_install(&host_config);
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "failed to install USB host: %s", esp_err_to_name(ret));
                goto out;
            }

            BaseType_t task_ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_host", USB_HOST_TASK_STACK_SIZE,
                                                          NULL, USB_HOST_TASK_PRIORITY, &s_usb_task_handle, 0);
            if (task_ret != pdTRUE) {
                if (ret == ESP_OK) {
                    (void)usb_host_uninstall();
                }
                ret = ESP_ERR_NO_MEM;
                goto out;
            }
            ESP_LOGI(TAG, "USB host installed");
        }
    }

    s_usb_ref_count++;
out:
    xSemaphoreGive(s_usb_lock);
    return ret;
}

static esp_err_t shared_usb_host_release(void)
{
    if (s_usb_lock == NULL) {
        return ESP_OK;
    }

    xSemaphoreTake(s_usb_lock, portMAX_DELAY);
    if (s_usb_ref_count > 0) {
        s_usb_ref_count--;
    }
    if (s_usb_ref_count == 0) {
        s_usb_stop_requested = true;
    }
    xSemaphoreGive(s_usb_lock);

    for (int i = 0; s_usb_ref_count == 0 && s_usb_task_handle != NULL && i < 100; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_OK;
}

static void uac_host_driver_callback(uint8_t addr, uint8_t iface_num,
                                     const uac_host_driver_event_t event, void *arg)
{
    (void)arg;
    if (s_uac_event_queue == NULL) {
        return;
    }

    const uac_driver_event_msg_t msg = {
        .addr = addr,
        .iface_num = iface_num,
        .event = event,
    };
    (void)xQueueSend(s_uac_event_queue, &msg, 0);
}

static void uac_event_task(void *arg)
{
    (void)arg;
    uac_driver_event_msg_t msg;

    while (true) {
        if (xQueueReceive(s_uac_event_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (msg.event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
            s_spk_dev_info = msg;
            s_spk_dev_found = true;
            ESP_LOGI(TAG, "UAC speaker found, addr: %u, iface: %u", msg.addr, msg.iface_num);
        } else if (msg.event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
            s_mic_dev_info = msg;
            s_mic_dev_found = true;
            ESP_LOGI(TAG, "UAC microphone found, addr: %u, iface: %u", msg.addr, msg.iface_num);
        }
    }
}

static esp_err_t uac_host_acquire(uint32_t preferred_rate)
{
    (void)preferred_rate;
    esp_err_t ret;

    ret = shared_usb_host_acquire();
    if (ret != ESP_OK) {
        return ret;
    }

    if (!s_uac_installed) {
        s_uac_event_queue = xQueueCreate(UAC_EVENT_QUEUE_LEN, sizeof(uac_driver_event_msg_t));
        ESP_GOTO_ON_FALSE(s_uac_event_queue != NULL, ESP_ERR_NO_MEM, fail, TAG, "failed to create UAC queue");

        const uac_host_driver_config_t uac_config = {
            .create_background_task = true,
            .task_priority = UAC_TASK_PRIORITY,
            .stack_size = UAC_TASK_STACK_SIZE,
            .core_id = 0,
            .callback = uac_host_driver_callback,
            .callback_arg = NULL,
        };
        ESP_GOTO_ON_ERROR(uac_host_install(&uac_config), fail, TAG, "failed to install UAC host");

        BaseType_t task_ret = xTaskCreatePinnedToCore(uac_event_task, "uac_events", UAC_TASK_STACK_SIZE,
                                                      NULL, UAC_TASK_PRIORITY,
                                                      &s_uac_task_handle, 0);
        ESP_GOTO_ON_FALSE(task_ret == pdTRUE, ESP_ERR_NO_MEM, fail_uninstall, TAG, "failed to create UAC task");

        s_uac_installed = true;
        ESP_LOGI(TAG, "UAC host installed");
    }

    s_uac_ref_count++;
    return ESP_OK;

fail_uninstall:
    (void)uac_host_uninstall();
fail:
    if (s_uac_event_queue) {
        vQueueDelete(s_uac_event_queue);
        s_uac_event_queue = NULL;
    }
    (void)shared_usb_host_release();
    return ret;
}

static esp_err_t uac_host_release(void)
{
    if (s_uac_ref_count > 0) {
        s_uac_ref_count--;
    }
    if (s_uac_ref_count == 0 && s_uac_installed) {
        if (s_uac_task_handle) {
            vTaskDelete(s_uac_task_handle);
            s_uac_task_handle = NULL;
        }
        (void)uac_host_uninstall();
        if (s_uac_event_queue) {
            vQueueDelete(s_uac_event_queue);
            s_uac_event_queue = NULL;
        }
        s_uac_installed = false;
        s_spk_dev_found = false;
        s_mic_dev_found = false;
        ESP_LOGI(TAG, "UAC host uninstalled");
    }
    return shared_usb_host_release();
}

static esp_err_t wait_for_uac_device(usb_device_kind_t kind, uac_driver_event_msg_t *out_info)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(UAC_CONNECT_TIMEOUT_MS);

    while (xTaskGetTickCount() < deadline) {
        const bool found = (kind == USB_DEVICE_KIND_UAC_SPK) ? s_spk_dev_found : s_mic_dev_found;
        if (found) {
            *out_info = (kind == USB_DEVICE_KIND_UAC_SPK) ? s_spk_dev_info : s_mic_dev_info;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return ESP_ERR_TIMEOUT;
}

static int usb_camera_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid camera handle");

#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    esp_err_t ret = shared_usb_host_acquire();
    if (ret != ESP_OK) {
        return ret;
    }

    const esp_video_init_usb_uvc_config_t usb_uvc_config = {
        .uvc = {
            .uvc_dev_num = USB_UVC_DEV_NUM,
            .task_stack = USB_UVC_TASK_STACK_SIZE,
            .task_priority = USB_UVC_TASK_PRIORITY,
            .task_affinity = 0,
        },
        .usb = {
            .init_usb_host_lib = false,
        },
    };
    const esp_video_init_config_t video_config = {
        .usb_uvc = &usb_uvc_config,
    };

    ret = esp_video_init(&video_config);
    if (ret != ESP_OK) {
        (void)shared_usb_host_release();
        return ret;
    }

    custom_usb_device_handle_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        (void)esp_video_deinit();
        (void)shared_usb_host_release();
        return ESP_ERR_NO_MEM;
    }
    handle->kind = USB_DEVICE_KIND_CAMERA;
    handle->handle.dev_path = ESP_VIDEO_USB_UVC_NAME(0);
    handle->handle.meta_path = "";
    *device_handle = &handle->handle;
    ESP_LOGI(TAG, "USB UVC camera initialized, dev_path: %s", handle->handle.dev_path);
    return ret;
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
    ESP_RETURN_ON_ERROR(shared_usb_host_release(), TAG, "failed to release USB host");
    ESP_LOGI(TAG, "USB UVC camera deinitialized");
    return ESP_OK;
#else
    (void)device_handle;
    return ESP_OK;
#endif
}

CUSTOM_DEVICE_IMPLEMENT(camera, usb_camera_init, usb_camera_deinit);


const static dev_audio_codec_config_t esp_bmgr_fake_audio_dac_cfg = {
    .name = "audio_dac",
    .chip = "none",
    .type = "audio_codec",
    .data_if_type = 0,
    .adc_enabled = true,
    .adc_max_channel = 0,
    .adc_channel_mask = 0x3,
    .adc_channel_labels = "",
    .adc_init_gain = 0,
    .dac_enabled = true,
    .dac_max_channel = 0,
    .dac_channel_mask = 0x0,
    .dac_init_gain = 10,
    .pa_cfg = {
        .name = NULL,
        .port = 0,
        .active_level = 0,
        .gain = 0.0,
    },
    .i2c_cfg = {
        .name = NULL,
        .port = 0,
        .address = 0,
        .frequency = 0,
    },
    .i2s_cfg = {
        .name = "i2s_audio_out",
        .port = 0,
        .clk_src = 0,
        .tx_aux_out_io = -1,
        .tx_aux_out_line = 0,
        .tx_aux_out_invert = false,
    },
    .adc_cfg = {
        .periph_name = NULL,
        .sample_rate_hz = 0,
        .max_store_buf_size = 0,
        .conv_frame_size = 0,
        .conv_mode = 0,
        .format = 0,
        .pattern_num = 0,
        .cfg_mode = 0,
        .cfg = {
            .single_unit = {
                .unit_id = 0,
                .atten = 0,
                .bit_width = 0,
                .channel_id = {},
            },
        },
    },
    .metadata = NULL,
    .metadata_size = 0,
    .mclk_enabled = false,
    .aec_enabled = false,
    .eq_enabled = false,
    .alc_enabled = false,
};


const static dev_audio_codec_config_t esp_bmgr_fake_audio_adc_cfg = {
    .name = "audio_adc",
    .chip = "none",
    .type = "audio_codec",
    .data_if_type = 0,
    .adc_enabled = true,
    .adc_max_channel = 0,
    .adc_channel_mask = 0x3,
    .adc_channel_labels = "",
    .adc_init_gain = 0,
    .dac_enabled = true,
    .dac_max_channel = 0,
    .dac_channel_mask = 0x0,
    .dac_init_gain = 10,
    .pa_cfg = {
        .name = NULL,
        .port = 0,
        .active_level = 0,
        .gain = 0.0,
    },
    .i2c_cfg = {
        .name = NULL,
        .port = 0,
        .address = 0,
        .frequency = 0,
    },
    .i2s_cfg = {
        .name = "i2s_audio_out",
        .port = 0,
        .clk_src = 0,
        .tx_aux_out_io = -1,
        .tx_aux_out_line = 0,
        .tx_aux_out_invert = false,
    },
    .adc_cfg = {
        .periph_name = NULL,
        .sample_rate_hz = 0,
        .max_store_buf_size = 0,
        .conv_frame_size = 0,
        .conv_mode = 0,
        .format = 0,
        .pattern_num = 0,
        .cfg_mode = 0,
        .cfg = {
            .single_unit = {
                .unit_id = 0,
                .atten = 0,
                .bit_width = 0,
                .channel_id = {},
            },
        },
    },
    .metadata = NULL,
    .metadata_size = 0,
    .mclk_enabled = false,
    .aec_enabled = false,
    .eq_enabled = false,
    .alc_enabled = false,
};

static int audio_dac_init(void *config, int cfg_size, void **device_handle)
{
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(config != NULL && device_handle != NULL ,
                        // cfg_size == sizeof(dev_custom_audio_dac_config_t),
                        ESP_ERR_INVALID_ARG, TAG, "invalid UAC speaker config");

    const dev_custom_audio_dac_config_t *cfg = (const dev_custom_audio_dac_config_t *)config;
    esp_err_t ret = uac_host_acquire(cfg->sample_rate_hz > 0 ? cfg->sample_rate_hz : 16000);
    if (ret != ESP_OK) {
        return ret;
    }

    uac_driver_event_msg_t uac_info = {0};
    ret = wait_for_uac_device(USB_DEVICE_KIND_UAC_SPK, &uac_info);
    if (ret != ESP_OK) {
        (void)uac_host_release();
        ESP_LOGE(TAG, "UAC speaker not found: %s", esp_err_to_name(ret));
        return ret;
    }

    const uac_codec_config_t uac_config = {
        .addr = uac_info.addr,
        .iface_num = uac_info.iface_num,
        .preferred_sample_rate = cfg->sample_rate_hz > 0 ? cfg->sample_rate_hz : 16000,
        .is_input = false,
    };
    dev_audio_codec_handles_t *codec_handles = uac_codec_new_handle(&uac_config);
    if (codec_handles == NULL) {
        (void)uac_host_release();
        return ESP_ERR_NO_MEM;
    }
    *device_handle = codec_handles;
    esp_board_device_override_config("audio_dac", (void *)&esp_bmgr_fake_audio_dac_cfg, sizeof(esp_bmgr_fake_audio_dac_cfg));
    ESP_LOGI(TAG, "UAC speaker initialized, codec_handles: %p", codec_handles);
    return ESP_OK;
}

static int audio_dac_deinit(void *device_handle)
{
    uac_codec_delete_handle((dev_audio_codec_handles_t *)device_handle);
    return uac_host_release();
}

CUSTOM_DEVICE_IMPLEMENT(audio_dac, audio_dac_init, audio_dac_deinit);

static int audio_adc_init(void *config, int cfg_size, void **device_handle)
{
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(config != NULL && device_handle != NULL ,
                        // cfg_size == sizeof(dev_custom_audio_adc_config_t),
                        ESP_ERR_INVALID_ARG, TAG, "invalid UAC microphone config");

    const dev_custom_audio_adc_config_t *cfg = (const dev_custom_audio_adc_config_t *)config;
    esp_err_t ret = uac_host_acquire(cfg->sample_rate_hz > 0 ? cfg->sample_rate_hz : 16000);
    if (ret != ESP_OK) {
        return ret;
    }

    uac_driver_event_msg_t uac_info = {0};
    ret = wait_for_uac_device(USB_DEVICE_KIND_UAC_MIC, &uac_info);
    if (ret != ESP_OK) {
        (void)uac_host_release();
        ESP_LOGE(TAG, "UAC microphone not found: %s", esp_err_to_name(ret));
        return ret;
    }

    const uac_codec_config_t uac_config = {
        .addr = uac_info.addr,
        .iface_num = uac_info.iface_num,
        .preferred_sample_rate = cfg->sample_rate_hz > 0 ? cfg->sample_rate_hz : 16000,
        .is_input = true,
    };
    dev_audio_codec_handles_t *codec_handles = uac_codec_new_handle(&uac_config);
    if (codec_handles == NULL) {
        (void)uac_host_release();
        return ESP_ERR_NO_MEM;
    }
    *device_handle = codec_handles;
    esp_board_device_override_config("audio_adc", (void *)&esp_bmgr_fake_audio_adc_cfg, sizeof(esp_bmgr_fake_audio_adc_cfg));
    ESP_LOGI(TAG, "UAC microphone initialized, codec_handles: %p", codec_handles);
    return ESP_OK;
}

static int audio_adc_deinit(void *device_handle)
{
    uac_codec_delete_handle((dev_audio_codec_handles_t *)device_handle);
    return uac_host_release();
}

CUSTOM_DEVICE_IMPLEMENT(audio_adc, audio_adc_init, audio_adc_deinit);

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));
    int ret = esp_lcd_new_panel_st7789(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "New st7789 panel failed");
        return ret;
    }
    return ESP_OK;
}
