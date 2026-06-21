/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hw_gfx_runtime.h"

#include <stdbool.h>
#include <string.h>

#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#if CONFIG_SOC_LCD_RGB_SUPPORTED
#include "esp_lcd_panel_rgb.h"
#endif
#if CONFIG_SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_mipi_dsi.h"
#endif

static const char *const TAG = "hw_gfx_runtime";

typedef enum {
    HW_GFX_RUNTIME_PANEL_IF_IO = 0,
    HW_GFX_RUNTIME_PANEL_IF_RGB,
    HW_GFX_RUNTIME_PANEL_IF_MIPI_DSI,
} hw_gfx_runtime_panel_if_t;

static gfx_handle_t s_gfx_handle;
static gfx_disp_t *s_disp;
static gfx_touch_t *s_touch;
static esp_lcd_panel_io_handle_t s_io_handle;
static esp_lcd_panel_handle_t s_panel_handle;
static esp_lcd_touch_handle_t s_touch_handle;
static uint16_t s_lcd_width;
static uint16_t s_lcd_height;
static bool s_swap_color = true;
static display_arbiter_owner_t s_flush_owner = DISPLAY_ARBITER_OWNER_NONE;
static display_arbiter_owner_t s_touch_owner = DISPLAY_ARBITER_OWNER_NONE;
static hw_gfx_runtime_panel_if_t s_panel_if = HW_GFX_RUNTIME_PANEL_IF_IO;
static hw_gfx_runtime_touch_event_cb_t s_touch_cb;
static void *s_touch_user_data;
static hw_gfx_runtime_active_cb_t s_active_cb;
static void *s_active_user_data;

#if GFX_MOTION_SCENE_SCHEMA_VERSION >= 3U
typedef gfx_coord_t hw_gfx_runtime_flush_coord_t;
#else
typedef int hw_gfx_runtime_flush_coord_t;
#endif

static bool hw_gfx_runtime_detect_swap_color(const dev_display_lcd_config_t *lcd_cfg)
{
    if (lcd_cfg == NULL || lcd_cfg->sub_type == NULL) {
        return true;
    }

    if (strcmp(lcd_cfg->sub_type, "dsi") == 0 ||
        strcmp(lcd_cfg->sub_type, "mipi_dsi") == 0 ||
        strcmp(lcd_cfg->sub_type, "rgb") == 0) {
        return false;
    }

    return true;
}

static hw_gfx_runtime_panel_if_t hw_gfx_runtime_detect_panel_if(const dev_display_lcd_config_t *lcd_cfg)
{
    if (lcd_cfg == NULL || lcd_cfg->sub_type == NULL) {
        return HW_GFX_RUNTIME_PANEL_IF_IO;
    }
    if (strcmp(lcd_cfg->sub_type, "dsi") == 0 ||
            strcmp(lcd_cfg->sub_type, "mipi_dsi") == 0) {
        return HW_GFX_RUNTIME_PANEL_IF_MIPI_DSI;
    }
    if (strcmp(lcd_cfg->sub_type, "rgb") == 0) {
        return HW_GFX_RUNTIME_PANEL_IF_RGB;
    }
    return HW_GFX_RUNTIME_PANEL_IF_IO;
}

static bool hw_gfx_runtime_flush_io_ready(esp_lcd_panel_io_handle_t panel_io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    gfx_disp_t *disp = (gfx_disp_t *)user_ctx;
    if (disp != NULL) {
        gfx_disp_flush_ready(disp, true);
    }
    return true;
}

#if CONFIG_SOC_LCD_RGB_SUPPORTED
static bool hw_gfx_runtime_flush_rgb_ready(esp_lcd_panel_handle_t panel,
                                           const esp_lcd_rgb_panel_event_data_t *edata,
                                           void *user_ctx)
{
    (void)panel;
    (void)edata;
    gfx_disp_t *disp = (gfx_disp_t *)user_ctx;
    if (disp != NULL) {
        gfx_disp_flush_ready(disp, true);
    }
    return true;
}
#endif

#if CONFIG_SOC_MIPI_DSI_SUPPORTED
static bool hw_gfx_runtime_flush_dpi_ready(esp_lcd_panel_handle_t panel_io,
                                           esp_lcd_dpi_panel_event_data_t *edata,
                                           void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    gfx_disp_t *disp = (gfx_disp_t *)user_ctx;
    if (disp != NULL) {
        gfx_disp_flush_ready(disp, true);
    }
    return true;
}
#endif

static esp_err_t hw_gfx_runtime_register_flush_callbacks(void)
{
    switch (s_panel_if) {
    case HW_GFX_RUNTIME_PANEL_IF_MIPI_DSI:
#if CONFIG_SOC_MIPI_DSI_SUPPORTED
    {
        ESP_RETURN_ON_FALSE(s_panel_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "panel handle missing");
        esp_lcd_dpi_panel_event_callbacks_t cbs = {0};
        cbs.on_color_trans_done = hw_gfx_runtime_flush_dpi_ready;
        return esp_lcd_dpi_panel_register_event_callbacks(s_panel_handle, &cbs, s_disp);
    }
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    case HW_GFX_RUNTIME_PANEL_IF_RGB:
#if CONFIG_SOC_LCD_RGB_SUPPORTED
    {
        ESP_RETURN_ON_FALSE(s_panel_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "panel handle missing");
        const esp_lcd_rgb_panel_event_callbacks_t cbs = {
            .on_color_trans_done = hw_gfx_runtime_flush_rgb_ready,
        };
        return esp_lcd_rgb_panel_register_event_callbacks(s_panel_handle, &cbs, s_disp);
    }
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    case HW_GFX_RUNTIME_PANEL_IF_IO:
    default: {
        ESP_RETURN_ON_FALSE(s_io_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "panel io handle missing");
        const esp_lcd_panel_io_callbacks_t cbs = {
            .on_color_trans_done = hw_gfx_runtime_flush_io_ready,
        };
        return esp_lcd_panel_io_register_event_callbacks(s_io_handle, &cbs, s_disp);
    }
    }
}

static void hw_gfx_runtime_unregister_flush_callbacks(void)
{
    switch (s_panel_if) {
    case HW_GFX_RUNTIME_PANEL_IF_MIPI_DSI:
#if CONFIG_SOC_MIPI_DSI_SUPPORTED
        if (s_panel_handle != NULL) {
            const esp_lcd_dpi_panel_event_callbacks_t cbs = {0};
            esp_lcd_dpi_panel_register_event_callbacks(s_panel_handle, &cbs, NULL);
        }
#endif
        break;
    case HW_GFX_RUNTIME_PANEL_IF_RGB:
#if CONFIG_SOC_LCD_RGB_SUPPORTED
        if (s_panel_handle != NULL) {
            const esp_lcd_rgb_panel_event_callbacks_t cbs = {0};
            esp_lcd_rgb_panel_register_event_callbacks(s_panel_handle, &cbs, NULL);
        }
#endif
        break;
    case HW_GFX_RUNTIME_PANEL_IF_IO:
    default:
        if (s_io_handle != NULL) {
            const esp_lcd_panel_io_callbacks_t cbs = {0};
            esp_lcd_panel_io_register_event_callbacks(s_io_handle, &cbs, NULL);
        }
        break;
    }
}

static void hw_gfx_runtime_refresh_all(void)
{
    if (s_disp != NULL) {
        gfx_disp_refresh_all(s_disp);
    }
    if (s_gfx_handle != NULL) {
        esp_err_t err = gfx_refr_now(s_gfx_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "refresh after owner switch failed: %s", esp_err_to_name(err));
        }
    }
}

static void hw_gfx_runtime_on_display_owner_changed(display_arbiter_owner_t owner, void *user_ctx)
{
    (void)user_ctx;

    if (display_arbiter_owner_uses_emote_gfx(owner)) {
        esp_err_t err = hw_gfx_runtime_register_flush_callbacks();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restore flush callbacks failed: %s", esp_err_to_name(err));
            s_flush_owner = DISPLAY_ARBITER_OWNER_NONE;
        } else {
            s_flush_owner = owner;
            hw_gfx_runtime_refresh_all();
        }
    } else {
        s_flush_owner = DISPLAY_ARBITER_OWNER_NONE;
        hw_gfx_runtime_unregister_flush_callbacks();
    }

    if (s_active_cb != NULL) {
        s_active_cb(display_arbiter_owner_uses_emote_gfx(owner), s_active_user_data);
    }
}

static void hw_gfx_runtime_flush_cb(gfx_disp_t *disp,
                                    hw_gfx_runtime_flush_coord_t x1,
                                    hw_gfx_runtime_flush_coord_t y1,
                                    hw_gfx_runtime_flush_coord_t x2,
                                    hw_gfx_runtime_flush_coord_t y2,
                                    const void *data)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)gfx_disp_get_user_data(disp);

    if (panel == NULL || s_flush_owner == DISPLAY_ARBITER_OWNER_NONE ||
            !display_arbiter_is_owner(s_flush_owner)) {
        gfx_disp_flush_ready(disp, true);
        return;
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_draw_bitmap failed: %s", esp_err_to_name(err));
        gfx_disp_flush_ready(disp, true);
    }
}

static void hw_gfx_runtime_touch_cb(gfx_touch_t *touch, const gfx_touch_event_t *event, void *user_data)
{
    (void)user_data;

    if (s_touch_cb != NULL && s_touch_owner != DISPLAY_ARBITER_OWNER_NONE &&
            display_arbiter_is_owner(s_touch_owner)) {
        s_touch_cb(touch, event, s_touch_user_data);
    }
}

static esp_err_t hw_gfx_runtime_load_board_display(void)
{
#if !CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    return ESP_ERR_NOT_SUPPORTED;
#else
    void *lcd_handle = NULL;
    void *lcd_config = NULL;

    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle),
                        TAG, "get display_lcd handle failed");
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config),
                        TAG, "get display_lcd config failed");

    dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)lcd_handle;
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)lcd_config;

    ESP_RETURN_ON_FALSE(lcd_handles != NULL && lcd_cfg != NULL && lcd_handles->panel_handle != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "display_lcd handle/config is NULL");

    s_panel_handle = lcd_handles->panel_handle;
    s_io_handle = lcd_handles->io_handle;
    s_lcd_width = lcd_cfg->lcd_width;
    s_lcd_height = lcd_cfg->lcd_height;
    s_panel_if = hw_gfx_runtime_detect_panel_if(lcd_cfg);

    esp_err_t disp_ret = esp_lcd_panel_disp_on_off(s_panel_handle, true);
    if (disp_ret != ESP_OK) {
        ESP_LOGW(TAG, "display on failed: %s", esp_err_to_name(disp_ret));
    }
    ESP_LOGD(TAG, "display loaded: panel=%p io=%p size=%ux%u sub_type=%s",
             s_panel_handle, s_io_handle, (unsigned)s_lcd_width, (unsigned)s_lcd_height,
             lcd_cfg->sub_type != NULL ? lcd_cfg->sub_type : "<none>");
    return ESP_OK;
#endif
}

static esp_err_t hw_gfx_runtime_load_board_touch(void)
{
#if !CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
    return ESP_ERR_NOT_SUPPORTED;
#else
    void *touch_handle = NULL;

    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &touch_handle),
                        TAG, "get lcd_touch handle failed");

    dev_lcd_touch_handles_t *touch_handles = (dev_lcd_touch_handles_t *)touch_handle;
    ESP_RETURN_ON_FALSE(touch_handles != NULL && touch_handles->touch_handle != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "lcd_touch handle is NULL");

    s_touch_handle = touch_handles->touch_handle;
    return ESP_OK;
#endif
}

esp_err_t hw_gfx_runtime_init(void)
{
    esp_err_t ret;

    if (s_gfx_handle != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(hw_gfx_runtime_load_board_display(), TAG, "Failed to load board display");

    esp_err_t touch_ret = hw_gfx_runtime_load_board_touch();
    if (touch_ret != ESP_OK) {
        s_touch_handle = NULL;
        ESP_LOGI(TAG, "touch disabled: %s", esp_err_to_name(touch_ret));
    }

    gfx_core_config_t gfx_cfg = {
        .fps = 15,
        .task = GFX_EMOTE_INIT_CONFIG()
    };
    gfx_cfg.task.task_stack_caps = MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL;
    gfx_cfg.task.task_affinity = 0;
    gfx_cfg.task.task_priority = 7;
    gfx_cfg.task.task_stack = 5 * 1024;
    s_gfx_handle = gfx_emote_init(&gfx_cfg);
    ESP_GOTO_ON_FALSE(s_gfx_handle != NULL, ESP_FAIL, err, TAG, "Failed to initialize graphics system");

    void *lcd_config = NULL;
    if (esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config) == ESP_OK) {
        s_swap_color = hw_gfx_runtime_detect_swap_color((const dev_display_lcd_config_t *)lcd_config);
    }

    gfx_disp_config_t disp_cfg = {
        .h_res = s_lcd_width,
        .v_res = s_lcd_height,
        .flush_cb = hw_gfx_runtime_flush_cb,
        .update_cb = NULL,
        .user_data = (void *)s_panel_handle,
#if CONFIG_IDF_TARGET_ESP32S3
        .flags = { .swap = s_swap_color, .buff_dma = true, .buff_spiram = false, .double_buffer = true },
#elif CONFIG_IDF_TARGET_ESP32C5
        .flags = { .swap = s_swap_color, .buff_dma = true, .buff_spiram = false, .double_buffer = true },
#elif CONFIG_IDF_TARGET_ESP32P4
        .flags = { .swap = s_swap_color, .buff_dma = true, .buff_spiram = false, .double_buffer = true },
#endif
        .buffers = { .buf1 = NULL, .buf2 = NULL, .buf_pixels = (size_t)s_lcd_width * 8 },
    };
    s_disp = gfx_disp_add(s_gfx_handle, &disp_cfg);
    ESP_GOTO_ON_FALSE(s_disp != NULL, ESP_FAIL, err_gfx, TAG, "Failed to add display");

    display_arbiter_owner_t owner = display_arbiter_get_owner();
    if (display_arbiter_owner_uses_emote_gfx(owner)) {
        ret = hw_gfx_runtime_register_flush_callbacks();
        ESP_GOTO_ON_ERROR(ret, err_gfx, TAG, "register flush callbacks failed");
        s_flush_owner = owner;
    }
    ret = display_arbiter_set_owner_changed_callback(hw_gfx_runtime_on_display_owner_changed, NULL);
    ESP_GOTO_ON_ERROR(ret, err_gfx, TAG, "register owner callback failed");

    if (s_touch_handle != NULL) {
        gfx_touch_config_t touch_cfg = {
            .handle = s_touch_handle,
            .event_cb = hw_gfx_runtime_touch_cb,
            .disp = s_disp,
            .poll_ms = 50,
            .user_data = s_gfx_handle,
        };
        s_touch = gfx_touch_add(s_gfx_handle, &touch_cfg);
        ESP_GOTO_ON_FALSE(s_touch != NULL, ESP_FAIL, err_gfx, TAG, "Failed to add touch");
    } else {
        s_touch = NULL;
    }

    return ESP_OK;

err_gfx:
    (void)display_arbiter_set_owner_changed_callback(NULL, NULL);
    hw_gfx_runtime_unregister_flush_callbacks();
    if (s_gfx_handle != NULL) {
        gfx_emote_deinit(s_gfx_handle);
    }
err:
    s_gfx_handle = NULL;
    s_disp = NULL;
    s_touch = NULL;
    s_panel_handle = NULL;
    s_io_handle = NULL;
    s_touch_handle = NULL;
    s_lcd_width = 0;
    s_lcd_height = 0;
    s_swap_color = true;
    s_panel_if = HW_GFX_RUNTIME_PANEL_IF_IO;
    return ret;
}

void hw_gfx_runtime_deinit(void)
{
    (void)display_arbiter_set_owner_changed_callback(NULL, NULL);
    hw_gfx_runtime_unregister_flush_callbacks();
    if (s_gfx_handle != NULL) {
        gfx_emote_deinit(s_gfx_handle);
    }
    s_gfx_handle = NULL;
    s_disp = NULL;
    s_touch = NULL;
    s_panel_handle = NULL;
    s_io_handle = NULL;
    s_touch_handle = NULL;
    s_lcd_width = 0;
    s_lcd_height = 0;
    s_swap_color = true;
    s_panel_if = HW_GFX_RUNTIME_PANEL_IF_IO;
    s_flush_owner = DISPLAY_ARBITER_OWNER_NONE;
    s_touch_owner = DISPLAY_ARBITER_OWNER_NONE;
    s_touch_cb = NULL;
    s_touch_user_data = NULL;
    s_active_cb = NULL;
    s_active_user_data = NULL;
}

bool hw_gfx_runtime_is_ready(void)
{
    return s_gfx_handle != NULL && s_disp != NULL;
}

gfx_handle_t hw_gfx_runtime_handle(void)
{
    return s_gfx_handle;
}

gfx_disp_t *hw_gfx_runtime_display(void)
{
    return s_disp;
}

gfx_touch_t *hw_gfx_runtime_touch(void)
{
    return s_touch;
}

esp_lcd_panel_io_handle_t hw_gfx_runtime_panel_io(void)
{
    return s_io_handle;
}

esp_lcd_panel_handle_t hw_gfx_runtime_panel(void)
{
    return s_panel_handle;
}

uint16_t hw_gfx_runtime_width(void)
{
    return s_lcd_width;
}

uint16_t hw_gfx_runtime_height(void)
{
    return s_lcd_height;
}

bool hw_gfx_runtime_should_swap_color(void)
{
    return s_swap_color;
}

esp_err_t hw_gfx_runtime_lock(void)
{
    return gfx_emote_lock(s_gfx_handle);
}

void hw_gfx_runtime_unlock(void)
{
    gfx_emote_unlock(s_gfx_handle);
}

gfx_timer_handle_t hw_gfx_runtime_timer_create(gfx_timer_cb_t timer_cb, uint32_t period, void *user_data)
{
    return gfx_timer_create(s_gfx_handle, timer_cb, period, user_data);
}

void hw_gfx_runtime_set_touch_event_cb(display_arbiter_owner_t owner,
                                       hw_gfx_runtime_touch_event_cb_t cb,
                                       void *user_data)
{
    s_touch_owner = cb != NULL ? owner : DISPLAY_ARBITER_OWNER_NONE;
    s_touch_cb = cb;
    s_touch_user_data = user_data;
}

void hw_gfx_runtime_clear_touch_event_cb(display_arbiter_owner_t owner, void *user_data)
{
    if (s_touch_owner == owner && s_touch_user_data == user_data) {
        s_touch_owner = DISPLAY_ARBITER_OWNER_NONE;
        s_touch_cb = NULL;
        s_touch_user_data = NULL;
    }
}

void hw_gfx_runtime_set_active_cb(hw_gfx_runtime_active_cb_t cb, void *user_data)
{
    s_active_cb = cb;
    s_active_user_data = user_data;
}

void hw_gfx_runtime_clear_active_cb(void *user_data)
{
    if (s_active_user_data == user_data) {
        s_active_cb = NULL;
        s_active_user_data = NULL;
    }
}
