/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "devices/dev_display_lcd/dev_display_lcd.h"
#include "devices/dev_lcd_touch/dev_lcd_touch.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display_service";

#define DISPLAY_SERVICE_DEFAULT_BUFFER_LINES 40
#define DISPLAY_SERVICE_DEFAULT_TICK_MS 5
#define DISPLAY_SERVICE_DEFAULT_TASK_PERIOD_MS 10
#define DISPLAY_SERVICE_TASK_STACK 8192
#define DISPLAY_SERVICE_TASK_PRIO 5
#define DISPLAY_SERVICE_MAX_SESSIONS 6
#define DISPLAY_SERVICE_MAX_CLIENTS 4
#define DISPLAY_SERVICE_SCENE_OWNER_NAME_LEN DISPLAY_SERVICE_OWNER_NAME_LEN

typedef void (*display_service_scene_cleanup_cb_t)(void *owner_ctx, void *user_ctx);

typedef enum {
    DISPLAY_SERVICE_SCENE_FLAG_RESTORE_DEFAULT_ON_RELEASE = 1 << 0,
    DISPLAY_SERVICE_SCENE_FLAG_ALLOW_SYSTEM_OVERLAY       = 1 << 1,
} display_service_scene_flags_t;

typedef struct {
    const char *owner_name;
    void *owner_ctx;
    uint32_t flags;
} display_service_scene_config_t;

typedef struct {
    void *owner_ctx;
    char owner_name[DISPLAY_SERVICE_OWNER_NAME_LEN];
} display_service_client_t;

typedef struct {
    const char *owner_name;
    void *owner_ctx;
    display_service_config_t display_config;
} display_service_client_config_t;

struct display_service_session_t {
    bool active;
    bool suspended;
    uint32_t generation;
    display_service_mode_t mode;
    uint32_t flags;
    display_service_session_cleanup_cb_t cleanup_cb;
    display_service_session_resume_cb_t resume_cb;
    void *cleanup_user_ctx;
    void *resume_user_ctx;
    struct display_service_session_t *preempted_session;
    char owner_name[DISPLAY_SERVICE_OWNER_NAME_LEN];
};

typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_touch_handle_t touch;
    lv_display_t *display;
    lv_indev_t *touch_indev;
    lv_obj_t *default_screen;
    lv_obj_t *dummy_input_blocker;
    SemaphoreHandle_t dummy_mutex;
    const char *dummy_owner;
    struct display_service_session_t *dummy_session;
    display_service_touch_observer_cb_t touch_observer_cb;
    void *touch_observer_user_ctx;
    display_service_state_observer_cb_t state_observer_cb;
    void *state_observer_user_ctx;
    display_service_client_t clients[DISPLAY_SERVICE_MAX_CLIENTS];
    struct display_service_session_t sessions[DISPLAY_SERVICE_MAX_SESSIONS];
    uint32_t session_generation;
    size_t client_count;
    void *scene_owner;
    char scene_owner_name[DISPLAY_SERVICE_SCENE_OWNER_NAME_LEN];
    uint32_t scene_flags;
    bool started;
    bool adapter_initialized;
    bool adapter_started;
    bool dummy_draw_enabled;
    bool dummy_draw_suspended;
} display_service_state_t;

EXT_RAM_BSS_ATTR static display_service_state_t s_display;

static esp_err_t display_service_scene_acquire_ex(const display_service_scene_config_t *config);
static esp_err_t display_service_scene_load_screen(void *owner_ctx, lv_obj_t *screen);
static esp_err_t display_service_scene_load_screen_locked(void *owner_ctx, lv_obj_t *screen);
static esp_err_t display_service_scene_release(void *owner_ctx);
static esp_err_t display_service_scene_release_with_cleanup(void *owner_ctx,
                                                            display_service_scene_cleanup_cb_t cleanup_cb,
                                                            void *cleanup_user_ctx);
static bool display_service_scene_allows_system_overlay(void);
static esp_err_t display_service_preempt_raw_for(display_service_session_handle_t session);
static esp_err_t display_service_restore_preempted_raw(display_service_session_handle_t session);
static esp_err_t display_service_enter_dummy_draw(display_service_session_handle_t session);
static esp_err_t display_service_exit_dummy_draw(display_service_session_handle_t session);
static esp_err_t display_service_dummy_draw_blit(const char *owner,
                                                 int x_start,
                                                 int y_start,
                                                 int x_end,
                                                 int y_end,
                                                 const void *frame_buffer,
                                                 bool wait);
static void display_service_dummy_draw_suspend_locked(void);
static void display_service_dummy_draw_resume_locked(void);

static bool display_service_dummy_owner_matches(const char *owner)
{
    return owner != NULL && s_display.dummy_owner != NULL &&
           strcmp(s_display.dummy_owner, owner) == 0;
}

static esp_err_t display_service_dummy_lock(void)
{
    ESP_RETURN_ON_FALSE(s_display.dummy_mutex != NULL, ESP_ERR_INVALID_STATE,
                        TAG, "dummy draw mutex missing");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_display.dummy_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "dummy draw mutex timeout");
    return ESP_OK;
}

static void display_service_dummy_unlock(void)
{
    if (s_display.dummy_mutex != NULL) {
        xSemaphoreGive(s_display.dummy_mutex);
    }
}

static void display_service_clear_scene_locked(void)
{
    s_display.scene_owner = NULL;
    s_display.scene_owner_name[0] = '\0';
    s_display.scene_flags = 0;
}

static bool display_service_session_slot_contains(const struct display_service_session_t *session)
{
    return session >= &s_display.sessions[0] &&
           session < &s_display.sessions[DISPLAY_SERVICE_MAX_SESSIONS];
}

static bool display_service_session_valid_unlocked(display_service_session_handle_t session)
{
    return session != NULL && display_service_session_slot_contains(session) && session->active;
}

static struct display_service_session_t *display_service_alloc_session_slot(void)
{
    for (size_t i = 0; i < DISPLAY_SERVICE_MAX_SESSIONS; i++) {
        if (!s_display.sessions[i].active) {
            return &s_display.sessions[i];
        }
    }
    return NULL;
}

static uint32_t display_service_session_flags_to_scene_flags(uint32_t flags)
{
    uint32_t scene_flags = 0;

    if (flags & DISPLAY_SERVICE_SESSION_FLAG_RESTORE_DEFAULT_ON_RELEASE) {
        scene_flags |= DISPLAY_SERVICE_SCENE_FLAG_RESTORE_DEFAULT_ON_RELEASE;
    }
    if (flags & DISPLAY_SERVICE_SESSION_FLAG_ALLOW_SYSTEM_OVERLAY) {
        scene_flags |= DISPLAY_SERVICE_SCENE_FLAG_ALLOW_SYSTEM_OVERLAY;
    }
    return scene_flags;
}

static void display_service_session_scene_cleanup_bridge(void *owner_ctx, void *user_ctx)
{
    display_service_session_handle_t session = (display_service_session_handle_t)owner_ctx;
    (void)user_ctx;

    if (display_service_session_valid_unlocked(session) && session->cleanup_cb != NULL) {
        session->cleanup_cb(session, session->cleanup_user_ctx);
    }
}

static bool display_service_should_auto_stop_locked(void)
{
    return s_display.started &&
           s_display.client_count == 0 &&
           s_display.scene_owner == NULL &&
           !s_display.dummy_draw_enabled;
}

static int display_service_find_client_locked(void *owner_ctx)
{
    for (size_t i = 0; i < s_display.client_count; i++) {
        if (s_display.clients[i].owner_ctx == owner_ctx) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t display_service_add_client_locked(const display_service_client_config_t *config)
{
    int index = display_service_find_client_locked(config->owner_ctx);

    if (index >= 0) {
        strlcpy(s_display.clients[index].owner_name,
                (config->owner_name && config->owner_name[0]) ? config->owner_name : "client",
                sizeof(s_display.clients[index].owner_name));
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(s_display.client_count < DISPLAY_SERVICE_MAX_CLIENTS,
                        ESP_ERR_NO_MEM, TAG, "display client slots exhausted");
    s_display.clients[s_display.client_count].owner_ctx = config->owner_ctx;
    strlcpy(s_display.clients[s_display.client_count].owner_name,
            (config->owner_name && config->owner_name[0]) ? config->owner_name : "client",
            sizeof(s_display.clients[s_display.client_count].owner_name));
    s_display.client_count++;
    return ESP_OK;
}

static bool display_service_remove_client_locked(void *owner_ctx)
{
    int index = display_service_find_client_locked(owner_ctx);

    if (index < 0) {
        return false;
    }
    size_t pos = (size_t)index;
    if (pos + 1 < s_display.client_count) {
        memmove(&s_display.clients[pos],
                &s_display.clients[pos + 1],
                (s_display.client_count - pos - 1) * sizeof(s_display.clients[0]));
    }
    s_display.client_count--;
    memset(&s_display.clients[s_display.client_count], 0, sizeof(s_display.clients[0]));
    return true;
}

static void display_service_notify_touch_observer(const display_service_touch_sample_t *sample)
{
    if (s_display.touch_observer_cb != NULL && sample != NULL) {
        s_display.touch_observer_cb(sample, s_display.touch_observer_user_ctx);
    }
}

static void display_service_notify_state_observer(display_service_state_event_t event)
{
    display_service_state_observer_cb_t cb = NULL;
    void *user_ctx = NULL;

    if (display_service_lock() == ESP_OK) {
        cb = s_display.state_observer_cb;
        user_ctx = s_display.state_observer_user_ctx;
        display_service_unlock();
    }

    if (cb != NULL) {
        cb(event, user_ctx);
    }
}

static esp_err_t display_service_create_dummy_input_blocker_locked(void)
{
    if (s_display.dummy_input_blocker != NULL) {
        lv_obj_move_foreground(s_display.dummy_input_blocker);
        return ESP_OK;
    }
    s_display.dummy_input_blocker = lv_obj_create(lv_layer_top());
    ESP_RETURN_ON_FALSE(s_display.dummy_input_blocker != NULL, ESP_ERR_NO_MEM,
                        TAG, "create dummy input blocker failed");
    lv_obj_remove_style_all(s_display.dummy_input_blocker);
    lv_obj_set_size(s_display.dummy_input_blocker, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_display.dummy_input_blocker, 0, 0);
    lv_obj_set_style_bg_opa(s_display.dummy_input_blocker, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_display.dummy_input_blocker, LV_OBJ_FLAG_SCROLLABLE);
    /* Keep dummy draw touches from reaching the underlying LVGL UI. */
    lv_obj_add_flag(s_display.dummy_input_blocker, LV_OBJ_FLAG_CLICKABLE);
    return ESP_OK;
}

static void display_service_delete_dummy_input_blocker_locked(void)
{
    if (s_display.dummy_input_blocker != NULL) {
        lv_obj_delete(s_display.dummy_input_blocker);
        s_display.dummy_input_blocker = NULL;
    }
}

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_DSI_SUPPORT || \
    CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT || \
    CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_3WIRE_SPI_SUPPORT
static esp_lv_adapter_rotation_t display_service_get_rotation(const dev_display_lcd_config_t *lcd_cfg)
{
    if (lcd_cfg->swap_xy) {
        return lcd_cfg->mirror_x ? ESP_LV_ADAPTER_ROTATE_90 : ESP_LV_ADAPTER_ROTATE_270;
    }
    return (lcd_cfg->mirror_x || lcd_cfg->mirror_y) ?
           ESP_LV_ADAPTER_ROTATE_180 : ESP_LV_ADAPTER_ROTATE_0;
}

static esp_lv_adapter_tear_avoid_mode_t display_service_get_tear_mode(uint8_t num_fbs)
{
    if (num_fbs >= 3) {
        return ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
    }
    if (num_fbs == 2) {
        return ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL;
    }
    return ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
}
#endif

static lv_display_t *display_service_register_adapter_display(const dev_display_lcd_config_t *lcd_cfg,
                                                              const dev_display_lcd_handles_t *lcd_handles,
                                                              uint32_t buffer_lines)
{
    esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    esp_lv_adapter_display_config_t disp_cfg;

    if (!lcd_cfg || !lcd_handles || !lcd_handles->panel_handle) {
        return NULL;
    }
    if (strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_DSI) == 0) {
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_DSI_SUPPORT
        rotation = display_service_get_rotation(lcd_cfg);
        disp_cfg = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(lcd_handles->panel_handle,
                                                              lcd_handles->io_handle,
                                                              lcd_cfg->lcd_width,
                                                              lcd_cfg->lcd_height,
                                                              rotation);
        disp_cfg.tear_avoid_mode = display_service_get_tear_mode(lcd_cfg->sub_cfg.dsi.dpi_config.num_fbs);
#else
        return NULL;
#endif
    } else if (strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB) == 0 ||
               strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB_3WIRE_SPI) == 0) {
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT || CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_3WIRE_SPI_SUPPORT
        rotation = display_service_get_rotation(lcd_cfg);
        disp_cfg = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(lcd_handles->panel_handle,
                                                             lcd_handles->io_handle,
                                                             lcd_cfg->lcd_width,
                                                             lcd_cfg->lcd_height,
                                                             rotation);
        if (strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB_3WIRE_SPI) == 0) {
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_3WIRE_SPI_SUPPORT
            disp_cfg.tear_avoid_mode =
                display_service_get_tear_mode(lcd_cfg->sub_cfg.rgb_3wire_spi.rgb_panel_config.num_fbs);
#endif
        } else {
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT
            disp_cfg.tear_avoid_mode = display_service_get_tear_mode(lcd_cfg->sub_cfg.rgb.panel_config.num_fbs);
#endif
        }
#else
        return NULL;
#endif
    } else if (strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_SPI) == 0 ||
               strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_I80) == 0 ||
               strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_PARLIO) == 0) {
#ifdef CONFIG_SPIRAM
        disp_cfg = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(lcd_handles->panel_handle,
                                                                        lcd_handles->io_handle,
                                                                        lcd_cfg->lcd_width,
                                                                        lcd_cfg->lcd_height,
                                                                        rotation);
#else
        disp_cfg = ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(lcd_handles->panel_handle,
                                                                           lcd_handles->io_handle,
                                                                           lcd_cfg->lcd_width,
                                                                           lcd_cfg->lcd_height,
                                                                           rotation);
#endif
    } else {
        ESP_LOGE(TAG, "unsupported LCD sub_type: %s", lcd_cfg->sub_type ? lcd_cfg->sub_type : "(null)");
        return NULL;
    }
    if (buffer_lines > 0 && buffer_lines <= lcd_cfg->lcd_height) {
        disp_cfg.profile.buffer_height = (uint16_t)buffer_lines;
    }
    ESP_LOGI(TAG, "register LVGL adapter display: sub_type=%s size=%ux%u rotation=%d tear=%d",
             lcd_cfg->sub_type, lcd_cfg->lcd_width, lcd_cfg->lcd_height,
             (int)rotation, (int)disp_cfg.tear_avoid_mode);
    return esp_lv_adapter_register_display(&disp_cfg);
}

static void display_service_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)lv_indev_get_user_data(indev);
    esp_lcd_touch_point_data_t point;
    uint8_t point_count = 0;
    display_service_touch_sample_t sample = {0};

    if (!tp) {
        data->state = LV_INDEV_STATE_RELEASED;
        display_service_notify_touch_observer(&sample);
        return;
    }
    (void)esp_lcd_touch_read_data(tp);
    if (esp_lcd_touch_get_data(tp, &point, &point_count, 1) == ESP_OK && point_count > 0) {
        data->point.x = (int32_t)point.x;
        data->point.y = (int32_t)point.y;
        data->state = LV_INDEV_STATE_PRESSED;
        sample.pressed = true;
        sample.x = data->point.x;
        sample.y = data->point.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    display_service_notify_touch_observer(&sample);
}

static esp_err_t display_service_load_board_display(dev_display_lcd_config_t **out_lcd_cfg,
                                                    dev_display_lcd_handles_t **out_lcd_handles)
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

    ESP_RETURN_ON_FALSE(lcd_handles && lcd_cfg && lcd_handles->panel_handle,
                        ESP_ERR_INVALID_STATE, TAG, "display_lcd handle/config invalid");

    if (out_lcd_cfg) {
        *out_lcd_cfg = lcd_cfg;
    }
    if (out_lcd_handles) {
        *out_lcd_handles = lcd_handles;
    }
    s_display.panel = lcd_handles->panel_handle;
    s_display.io = lcd_handles->io_handle;
    (void)esp_lcd_panel_disp_on_off(s_display.panel, true);
    return ESP_OK;
#endif
}

static void display_service_load_board_touch(void)
{
#if CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
    void *touch_handle = NULL;

    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &touch_handle) != ESP_OK) {
        ESP_LOGI(TAG, "touch disabled: handle not found");
        return;
    }
    dev_lcd_touch_handles_t *touch_handles = (dev_lcd_touch_handles_t *)touch_handle;
    if (touch_handles && touch_handles->touch_handle) {
        s_display.touch = touch_handles->touch_handle;
    }
#else
    ESP_LOGI(TAG, "touch disabled: board touch support off");
#endif
}

esp_err_t display_service_start(const display_service_config_t *config)
{
    esp_err_t ret;
    uint32_t buffer_lines = config && config->buffer_lines ?
                            config->buffer_lines : DISPLAY_SERVICE_DEFAULT_BUFFER_LINES;
    uint32_t tick_ms = config && config->tick_ms ?
                       config->tick_ms : DISPLAY_SERVICE_DEFAULT_TICK_MS;
    dev_display_lcd_config_t *lcd_cfg = NULL;
    dev_display_lcd_handles_t *lcd_handles = NULL;

    if (s_display.started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(display_service_load_board_display(&lcd_cfg, &lcd_handles), TAG, "load display failed");
    display_service_load_board_touch();
    ESP_RETURN_ON_FALSE(buffer_lines > 0 && buffer_lines <= lcd_cfg->lcd_height,
                        ESP_ERR_INVALID_ARG, TAG, "invalid buffer lines");

    if (!s_display.dummy_mutex) {
        s_display.dummy_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_display.dummy_mutex != NULL, ESP_ERR_NO_MEM,
                            TAG, "create dummy draw mutex failed");
    }

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_priority = DISPLAY_SERVICE_TASK_PRIO;
    adapter_config.task_stack_size = DISPLAY_SERVICE_TASK_STACK;
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    adapter_config.task_core_id = 1;
#else
    adapter_config.task_core_id = 0;
#endif
    adapter_config.tick_period_ms = tick_ms;
    adapter_config.task_max_delay_ms = config && config->task_period_ms ?
                                       config->task_period_ms : DISPLAY_SERVICE_DEFAULT_TASK_PERIOD_MS;
    ESP_GOTO_ON_ERROR(esp_lv_adapter_init(&adapter_config), fail, TAG, "init LVGL adapter failed");
    s_display.adapter_initialized = true;

    s_display.display = display_service_register_adapter_display(lcd_cfg, lcd_handles, buffer_lines);
    ESP_GOTO_ON_FALSE(s_display.display != NULL, ESP_FAIL, fail, TAG, "register LVGL adapter display failed");

    ESP_GOTO_ON_ERROR(display_service_lock(), fail, TAG, "lock failed");
    if (s_display.touch) {
        s_display.touch_indev = lv_indev_create();
        if (s_display.touch_indev) {
            lv_indev_set_type(s_display.touch_indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(s_display.touch_indev, display_service_touch_read_cb);
            lv_indev_set_user_data(s_display.touch_indev, s_display.touch);
            lv_indev_set_display(s_display.touch_indev, s_display.display);
        } else {
            ESP_LOGW(TAG, "touch disabled: lv_indev_create failed");
        }
    }
    display_service_unlock();

    ESP_GOTO_ON_ERROR(esp_lv_adapter_start(), fail, TAG, "start LVGL adapter failed");
    s_display.adapter_started = true;
    s_display.started = true;

    ESP_LOGI(TAG, "started display service: %ux%u", lcd_cfg->lcd_width, lcd_cfg->lcd_height);
    return ESP_OK;

fail:
    display_service_stop();
    return ret;
}

void display_service_stop(void)
{
    if (s_display.adapter_initialized && display_service_lock() == ESP_OK) {
        s_display.started = false;
        display_service_delete_dummy_input_blocker_locked();
        if (s_display.touch_indev) {
            lv_indev_delete(s_display.touch_indev);
            s_display.touch_indev = NULL;
        }
        display_service_clear_scene_locked();
        memset(s_display.clients, 0, sizeof(s_display.clients));
        memset(s_display.sessions, 0, sizeof(s_display.sessions));
        s_display.client_count = 0;
        display_service_unlock();
    }
    if (s_display.dummy_draw_enabled && s_display.display) {
        (void)esp_lv_adapter_set_dummy_draw(s_display.display, false);
    }
    if (s_display.display) {
        esp_lv_adapter_unregister_display(s_display.display);
        s_display.display = NULL;
    }
    if (s_display.adapter_initialized) {
        (void)esp_lv_adapter_deinit();
        s_display.adapter_initialized = false;
        s_display.adapter_started = false;
    }
    if (s_display.dummy_mutex) {
        vSemaphoreDelete(s_display.dummy_mutex);
        s_display.dummy_mutex = NULL;
    }
    s_display.panel = NULL;
    s_display.io = NULL;
    s_display.touch = NULL;
    s_display.dummy_owner = NULL;
    s_display.dummy_session = NULL;
    s_display.touch_observer_cb = NULL;
    s_display.touch_observer_user_ctx = NULL;
    s_display.state_observer_cb = NULL;
    s_display.state_observer_user_ctx = NULL;
    memset(s_display.clients, 0, sizeof(s_display.clients));
    memset(s_display.sessions, 0, sizeof(s_display.sessions));
    s_display.client_count = 0;
    s_display.default_screen = NULL;
    s_display.scene_owner_name[0] = '\0';
    s_display.scene_flags = 0;
    s_display.dummy_draw_enabled = false;
    s_display.dummy_draw_suspended = false;
}

bool display_service_is_started(void)
{
    return s_display.started;
}

esp_err_t display_service_open(const display_service_session_config_t *config,
                               display_service_session_handle_t *ret_session)
{
    esp_err_t err;
    struct display_service_session_t *session = NULL;
    const char *owner_name;

    ESP_RETURN_ON_FALSE(config != NULL && ret_session != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "session config/output missing");
    ESP_RETURN_ON_FALSE(config->mode == DISPLAY_SERVICE_MODE_SHARED_LVGL ||
                        config->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL ||
                        config->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
                        ESP_ERR_INVALID_ARG, TAG, "invalid session mode");
    *ret_session = NULL;
    owner_name = (config->owner_name && config->owner_name[0]) ? config->owner_name : "session";

    err = display_service_start(&config->display_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open session display start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_RETURN_ON_ERROR(display_service_lock(), TAG, "lock failed");
    session = display_service_alloc_session_slot();
    if (session == NULL) {
        display_service_unlock();
        ESP_LOGE(TAG, "display session slots exhausted");
        return ESP_ERR_NO_MEM;
    }
    memset(session, 0, sizeof(*session));
    session->active = true;
    session->generation = ++s_display.session_generation;
    session->mode = config->mode;
    session->flags = config->flags;
    session->cleanup_cb = config->cleanup_cb;
    session->resume_cb = config->resume_cb;
    session->cleanup_user_ctx = config->user_ctx;
    session->resume_user_ctx = config->user_ctx;
    strlcpy(session->owner_name, owner_name, sizeof(session->owner_name));
    err = display_service_add_client_locked(&(display_service_client_config_t) {
        .owner_name = session->owner_name,
        .owner_ctx = session,
        .display_config = config->display_config,
    });
    display_service_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open session add client failed: %s", esp_err_to_name(err));
        session->active = false;
        return err;
    }

    if (config->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL) {
        err = display_service_scene_acquire_ex(&(display_service_scene_config_t) {
            .owner_name = session->owner_name,
            .owner_ctx = session,
            .flags = display_service_session_flags_to_scene_flags(config->flags),
        });
    } else if (config->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW) {
        err = display_service_preempt_raw_for(session);
        if (err == ESP_OK) {
            err = display_service_enter_dummy_draw(session);
        }
    }
    if (err != ESP_OK) {
        if (session->preempted_session != NULL) {
            (void)display_service_restore_preempted_raw(session);
        }
        ESP_LOGE(TAG, "open session exclusive enter failed: %s", esp_err_to_name(err));
        if (display_service_lock() == ESP_OK) {
            (void)display_service_remove_client_locked(session);
            display_service_unlock();
        }
        session->active = false;
        return err;
    }

    *ret_session = session;
    ESP_LOGI(TAG, "display session opened: owner=%s mode=%d", session->owner_name, (int)session->mode);
    return ESP_OK;
}

esp_err_t display_service_close(display_service_session_handle_t session)
{
    esp_err_t err = ESP_OK;
    display_service_mode_t mode;
    display_service_session_cleanup_cb_t cleanup_cb;
    void *cleanup_user_ctx;
    char owner_name[DISPLAY_SERVICE_OWNER_NAME_LEN];
    bool should_stop = false;

    ESP_RETURN_ON_FALSE(display_service_session_valid_unlocked(session),
                        ESP_ERR_INVALID_ARG, TAG, "invalid display session");
    mode = session->mode;
    cleanup_cb = session->cleanup_cb;
    cleanup_user_ctx = session->cleanup_user_ctx;
    strlcpy(owner_name, session->owner_name, sizeof(owner_name));

    if (mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW) {
        err = display_service_exit_dummy_draw(session);
    } else if (mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL) {
        err = cleanup_cb != NULL ?
              display_service_scene_release_with_cleanup(session,
                                                         display_service_session_scene_cleanup_bridge,
                                                         NULL) :
              display_service_scene_release(session);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "close session exclusive exit failed: %s", esp_err_to_name(err));
        return err;
    }
    if (mode != DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL && cleanup_cb != NULL) {
        cleanup_cb(session, cleanup_user_ctx);
    }

    if (mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW && session->preempted_session != NULL) {
        esp_err_t resume_err = display_service_restore_preempted_raw(session);
        if (resume_err != ESP_OK) {
            ESP_LOGE(TAG, "restore preempted display session failed: %s",
                     esp_err_to_name(resume_err));
            return resume_err;
        }
    }

    ESP_RETURN_ON_ERROR(display_service_lock(), TAG, "lock failed");
    (void)display_service_remove_client_locked(session);
    memset(session, 0, sizeof(*session));
    should_stop = display_service_should_auto_stop_locked();
    display_service_unlock();
    ESP_LOGI(TAG, "display session closed: owner=%s", owner_name);

    if (should_stop) {
        display_service_stop();
    }
    return ESP_OK;
}

bool display_service_session_is_valid(display_service_session_handle_t session)
{
    return display_service_session_valid_unlocked(session);
}

bool display_service_session_is_active(display_service_session_handle_t session)
{
    return display_service_session_is_valid(session);
}

display_service_mode_t display_service_session_mode(display_service_session_handle_t session)
{
    return display_service_session_is_valid(session) ? session->mode : DISPLAY_SERVICE_MODE_SHARED_LVGL;
}

const char *display_service_session_owner_name(display_service_session_handle_t session)
{
    return display_service_session_is_valid(session) ? session->owner_name : NULL;
}

esp_err_t display_service_session_load_screen_locked(display_service_session_handle_t session, lv_obj_t *screen)
{
    ESP_RETURN_ON_FALSE(display_service_session_valid_unlocked(session),
                        ESP_ERR_INVALID_ARG, TAG, "invalid display session");
    ESP_RETURN_ON_FALSE(session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL,
                        ESP_ERR_INVALID_STATE, TAG, "session is not exclusive LVGL");
    return display_service_scene_load_screen_locked(session, screen);
}

esp_err_t display_service_session_load_screen(display_service_session_handle_t session, lv_obj_t *screen)
{
    ESP_RETURN_ON_FALSE(display_service_session_valid_unlocked(session),
                        ESP_ERR_INVALID_ARG, TAG, "invalid display session");
    ESP_RETURN_ON_FALSE(session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL,
                        ESP_ERR_INVALID_STATE, TAG, "session is not exclusive LVGL");
    return display_service_scene_load_screen(session, screen);
}

lv_display_t *display_service_session_display(display_service_session_handle_t session)
{
    return display_service_session_is_valid(session) ? s_display.display : NULL;
}

esp_err_t display_service_session_raw_blit(display_service_session_handle_t session,
                                           const display_service_raw_blit_t *blit)
{
    ESP_RETURN_ON_FALSE(display_service_session_valid_unlocked(session),
                        ESP_ERR_INVALID_ARG, TAG, "invalid display session");
    ESP_RETURN_ON_FALSE(blit != NULL, ESP_ERR_INVALID_ARG, TAG, "raw blit config missing");
    ESP_RETURN_ON_FALSE(session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
                        ESP_ERR_INVALID_STATE, TAG, "session is not exclusive raw");
    return display_service_dummy_draw_blit(session->owner_name,
                                           blit->x_start,
                                           blit->y_start,
                                           blit->x_end,
                                           blit->y_end,
                                           blit->frame_buffer,
                                           blit->wait);
}

bool display_service_has_exclusive_session(void)
{
    return s_display.scene_owner != NULL || s_display.dummy_draw_enabled;
}

bool display_service_exclusive_allows_system_overlay(void)
{
    if (s_display.scene_owner != NULL) {
        return display_service_scene_allows_system_overlay();
    }
    if (s_display.dummy_draw_enabled &&
            display_service_session_valid_unlocked(s_display.dummy_session)) {
        return (s_display.dummy_session->flags &
                DISPLAY_SERVICE_SESSION_FLAG_ALLOW_SYSTEM_OVERLAY) != 0;
    }
    return true;
}

esp_err_t display_service_lock(void)
{
    return esp_lv_adapter_lock(1000);
}

void display_service_unlock(void)
{
    esp_lv_adapter_unlock();
}

esp_err_t display_service_set_touch_observer(display_service_touch_observer_cb_t cb,
                                             void *user_ctx)
{
    ESP_RETURN_ON_ERROR(display_service_lock(), TAG, "lock failed");
    s_display.touch_observer_cb = cb;
    s_display.touch_observer_user_ctx = user_ctx;
    display_service_unlock();
    return ESP_OK;
}

esp_err_t display_service_set_state_observer(display_service_state_observer_cb_t cb,
                                             void *user_ctx)
{
    ESP_RETURN_ON_ERROR(display_service_lock(), TAG, "lock failed");
    s_display.state_observer_cb = cb;
    s_display.state_observer_user_ctx = user_ctx;
    display_service_unlock();
    return ESP_OK;
}

esp_err_t display_service_set_default_screen(lv_obj_t *screen)
{
    ESP_RETURN_ON_ERROR(display_service_lock(), TAG, "lock failed");
    display_service_set_default_screen_locked(screen);
    display_service_unlock();
    return ESP_OK;
}

void display_service_set_default_screen_locked(lv_obj_t *screen)
{
    s_display.default_screen = screen;
}

lv_obj_t *display_service_default_screen(void)
{
    return s_display.default_screen;
}

static esp_err_t display_service_scene_acquire_ex(const display_service_scene_config_t *config)
{
    bool acquired = false;

    ESP_RETURN_ON_FALSE(config != NULL && config->owner_ctx != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "scene owner missing");
    ESP_RETURN_ON_FALSE(s_display.started, ESP_ERR_INVALID_STATE, TAG, "service not started");
    ESP_RETURN_ON_ERROR(display_service_lock(), TAG, "lock failed");
    if (s_display.dummy_draw_enabled) {
        display_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_display.scene_owner != NULL && s_display.scene_owner != config->owner_ctx) {
        display_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_display.scene_owner == NULL) {
        acquired = true;
        s_display.scene_owner = config->owner_ctx;
        s_display.scene_flags = config->flags;
        strlcpy(s_display.scene_owner_name,
                (config->owner_name && config->owner_name[0]) ? config->owner_name : "scene",
                sizeof(s_display.scene_owner_name));
    } else {
        s_display.scene_flags = config->flags;
        strlcpy(s_display.scene_owner_name,
                (config->owner_name && config->owner_name[0]) ? config->owner_name : "scene",
                sizeof(s_display.scene_owner_name));
    }
    display_service_unlock();
    if (acquired) {
        display_service_notify_state_observer(DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_LVGL_ENTERED);
    }
    return ESP_OK;
}

static esp_err_t display_service_scene_load_screen_locked(void *owner_ctx, lv_obj_t *screen)
{
    ESP_RETURN_ON_FALSE(owner_ctx != NULL, ESP_ERR_INVALID_ARG, TAG, "scene owner missing");
    ESP_RETURN_ON_FALSE(screen != NULL, ESP_ERR_INVALID_ARG, TAG, "scene screen missing");
    ESP_RETURN_ON_FALSE(s_display.started, ESP_ERR_INVALID_STATE, TAG, "service not started");
    ESP_RETURN_ON_FALSE(s_display.scene_owner == owner_ctx,
                        ESP_ERR_INVALID_STATE, TAG, "scene owner mismatch");

    lv_screen_load(screen);
    return ESP_OK;
}

static esp_err_t display_service_scene_load_screen(void *owner_ctx, lv_obj_t *screen)
{
    esp_err_t err;

    ESP_RETURN_ON_ERROR(display_service_lock(), TAG, "lock failed");
    err = display_service_scene_load_screen_locked(owner_ctx, screen);
    display_service_unlock();
    return err;
}

static esp_err_t display_service_scene_release_internal(void *owner_ctx,
                                                        display_service_scene_cleanup_cb_t cleanup_cb,
                                                        void *cleanup_user_ctx)
{
    bool released = false;
    bool should_stop = false;

    ESP_RETURN_ON_FALSE(owner_ctx != NULL, ESP_ERR_INVALID_ARG, TAG, "scene owner missing");
    ESP_RETURN_ON_FALSE(s_display.started, ESP_ERR_INVALID_STATE, TAG, "service not started");
    ESP_RETURN_ON_ERROR(display_service_lock(), TAG, "lock failed");
    if (s_display.scene_owner == owner_ctx) {
        released = true;
        if ((s_display.scene_flags & DISPLAY_SERVICE_SCENE_FLAG_RESTORE_DEFAULT_ON_RELEASE) &&
                s_display.default_screen != NULL) {
            lv_screen_load(s_display.default_screen);
        }
        if (cleanup_cb != NULL) {
            cleanup_cb(owner_ctx, cleanup_user_ctx);
        }
        display_service_clear_scene_locked();
        should_stop = display_service_should_auto_stop_locked();
    }
    display_service_unlock();
    if (released) {
        display_service_notify_state_observer(DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_LVGL_EXITED);
    }
    if (should_stop) {
        display_service_stop();
    }
    return ESP_OK;
}

static esp_err_t display_service_scene_release(void *owner_ctx)
{
    return display_service_scene_release_internal(owner_ctx, NULL, NULL);
}

static esp_err_t display_service_scene_release_with_cleanup(void *owner_ctx,
                                                            display_service_scene_cleanup_cb_t cleanup_cb,
                                                            void *cleanup_user_ctx)
{
    return display_service_scene_release_internal(owner_ctx, cleanup_cb, cleanup_user_ctx);
}

static bool display_service_scene_allows_system_overlay(void)
{
    return (s_display.scene_flags & DISPLAY_SERVICE_SCENE_FLAG_ALLOW_SYSTEM_OVERLAY) != 0;
}

static void display_service_notify_session_resume(display_service_session_handle_t session)
{
    if (display_service_session_valid_unlocked(session) && session->resume_cb != NULL) {
        session->resume_cb(session, session->resume_user_ctx);
    }
}

static esp_err_t display_service_preempt_raw_for(display_service_session_handle_t session)
{
    esp_err_t ret = ESP_OK;
    bool preempted = false;

    ESP_RETURN_ON_FALSE(display_service_session_valid_unlocked(session),
                        ESP_ERR_INVALID_ARG, TAG, "preempting session invalid");
    ESP_RETURN_ON_ERROR(display_service_dummy_lock(), TAG, "dummy lock failed");
    ret = display_service_lock();
    if (ret != ESP_OK) {
        display_service_dummy_unlock();
        return ret;
    }
    if (!s_display.dummy_draw_enabled) {
        display_service_unlock();
        display_service_dummy_unlock();
        return ESP_OK;
    }

    display_service_session_handle_t current = s_display.dummy_session;
    if (!display_service_session_valid_unlocked(current) ||
            current->mode != DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW ||
            !(current->flags & DISPLAY_SERVICE_SESSION_FLAG_PREEMPTIBLE) ||
            current->suspended || session->preempted_session != NULL) {
        display_service_unlock();
        display_service_dummy_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    display_service_delete_dummy_input_blocker_locked();
    if (!s_display.dummy_draw_suspended && s_display.display != NULL) {
        ret = esp_lv_adapter_set_dummy_draw(s_display.display, false);
    }
    if (ret == ESP_OK) {
        current->suspended = true;
        session->preempted_session = current;
        s_display.dummy_owner = NULL;
        s_display.dummy_session = NULL;
        s_display.dummy_draw_enabled = false;
        s_display.dummy_draw_suspended = false;
        preempted = true;
        ESP_LOGI(TAG, "raw display preempted: owner=%s by=%s",
                 current->owner_name, session->owner_name);
    } else {
        (void)display_service_create_dummy_input_blocker_locked();
    }
    display_service_unlock();
    display_service_dummy_unlock();

    if (preempted) {
        display_service_notify_state_observer(DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_EXITED);
    }
    return ret;
}

static esp_err_t display_service_enter_dummy_draw(display_service_session_handle_t session)
{
    esp_err_t ret;
    bool entered = false;
    const char *owner;

    ESP_RETURN_ON_FALSE(display_service_session_valid_unlocked(session),
                        ESP_ERR_INVALID_ARG, TAG, "dummy session invalid");
    owner = session->owner_name;
    ESP_RETURN_ON_ERROR(display_service_dummy_lock(), TAG, "dummy lock failed");
    ret = display_service_lock();
    if (ret != ESP_OK) {
        display_service_dummy_unlock();
        return ret;
    }
    if (!s_display.started || s_display.display == NULL) {
        display_service_unlock();
        display_service_dummy_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_display.scene_owner != NULL) {
        display_service_unlock();
        display_service_dummy_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_display.dummy_session != NULL && s_display.dummy_session != session) {
        display_service_unlock();
        display_service_dummy_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_display.dummy_draw_enabled) {
        display_service_unlock();
        display_service_dummy_unlock();
        return ESP_OK;
    }

    if (ret == ESP_OK) {
        ret = display_service_create_dummy_input_blocker_locked();
    }
    if (ret == ESP_OK) {
        ret = esp_lv_adapter_set_dummy_draw(s_display.display, true);
    }
    if (ret == ESP_OK) {
        s_display.dummy_owner = owner;
        s_display.dummy_session = session;
        s_display.dummy_draw_enabled = true;
        s_display.dummy_draw_suspended = false;
        entered = true;
        ESP_LOGI(TAG, "dummy draw entered: owner=%s", owner);
    } else {
        display_service_delete_dummy_input_blocker_locked();
    }
    display_service_unlock();
    display_service_dummy_unlock();
    if (entered) {
        display_service_notify_state_observer(DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_ENTERED);
    }
    return ret;
}

static esp_err_t display_service_exit_dummy_draw(display_service_session_handle_t session)
{
    esp_err_t ret = ESP_OK;
    bool exited = false;
    bool should_stop = false;
    const char *owner;

    ESP_RETURN_ON_FALSE(display_service_session_valid_unlocked(session),
                        ESP_ERR_INVALID_ARG, TAG, "dummy session invalid");
    ESP_RETURN_ON_FALSE(!session->suspended, ESP_ERR_INVALID_STATE,
                        TAG, "cannot close a preempted raw session");
    owner = session->owner_name;
    ESP_RETURN_ON_ERROR(display_service_dummy_lock(), TAG, "dummy lock failed");
    if (!s_display.dummy_draw_enabled) {
        display_service_dummy_unlock();
        return ESP_OK;
    }
    if (s_display.dummy_session != session || !display_service_dummy_owner_matches(owner)) {
        display_service_dummy_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    ret = display_service_lock();
    if (ret != ESP_OK) {
        display_service_dummy_unlock();
        return ret;
    }
    display_service_delete_dummy_input_blocker_locked();
    if (s_display.display != NULL && !s_display.dummy_draw_suspended) {
        ret = esp_lv_adapter_set_dummy_draw(s_display.display, false);
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "dummy draw exited: owner=%s", owner);
        s_display.dummy_owner = NULL;
        s_display.dummy_session = NULL;
        s_display.dummy_draw_enabled = false;
        s_display.dummy_draw_suspended = false;
        should_stop = display_service_should_auto_stop_locked();
        exited = true;
    }
    display_service_unlock();
    display_service_dummy_unlock();
    if (exited) {
        display_service_notify_state_observer(DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_EXITED);
    }
    if (should_stop) {
        display_service_stop();
    }
    return ret;
}

static esp_err_t display_service_restore_preempted_raw(display_service_session_handle_t session)
{
    display_service_session_handle_t preempted;

    ESP_RETURN_ON_FALSE(display_service_session_valid_unlocked(session),
                        ESP_ERR_INVALID_ARG, TAG, "preempting session invalid");
    preempted = session->preempted_session;
    if (preempted == NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(display_service_session_valid_unlocked(preempted) && preempted->suspended,
                        ESP_ERR_INVALID_STATE, TAG, "preempted session invalid");

    esp_err_t err = display_service_enter_dummy_draw(preempted);
    if (err != ESP_OK) {
        return err;
    }
    preempted->suspended = false;
    session->preempted_session = NULL;
    ESP_LOGI(TAG, "raw display resumed: owner=%s", preempted->owner_name);
    display_service_notify_session_resume(preempted);
    return ESP_OK;
}

static esp_err_t display_service_dummy_draw_blit(const char *owner,
                                                 int x_start,
                                                 int y_start,
                                                 int x_end,
                                                 int y_end,
                                                 const void *frame_buffer,
                                                 bool wait)
{
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(owner != NULL, ESP_ERR_INVALID_ARG, TAG, "dummy owner missing");
    ESP_RETURN_ON_FALSE(frame_buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "frame buffer missing");
    ESP_RETURN_ON_ERROR(display_service_dummy_lock(), TAG, "dummy lock failed");
    if (!s_display.dummy_draw_enabled || !display_service_dummy_owner_matches(owner) ||
            s_display.display == NULL) {
        display_service_dummy_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_display.dummy_draw_suspended) {
        display_service_dummy_unlock();
        return ESP_OK;
    }
    ret = esp_lv_adapter_dummy_draw_blit(s_display.display, x_start, y_start, x_end, y_end, frame_buffer, wait);
    display_service_dummy_unlock();
    return ret;
}

static void display_service_dummy_draw_suspend_locked(void)
{
    if (!s_display.dummy_draw_enabled || s_display.dummy_draw_suspended || s_display.display == NULL) {
        return;
    }
    display_service_delete_dummy_input_blocker_locked();
    esp_err_t err = esp_lv_adapter_set_dummy_draw(s_display.display, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "suspend dummy draw failed: %s", esp_err_to_name(err));
        return;
    }
    s_display.dummy_draw_suspended = true;
    lv_obj_invalidate(lv_layer_top());
    ESP_LOGI(TAG, "dummy draw suspended");
}

static void display_service_dummy_draw_resume_locked(void)
{
    display_service_session_handle_t session = s_display.dummy_session;

    if (!s_display.dummy_draw_enabled || !s_display.dummy_draw_suspended || s_display.display == NULL) {
        return;
    }
    esp_err_t err = esp_lv_adapter_set_dummy_draw(s_display.display, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "resume dummy draw failed: %s", esp_err_to_name(err));
        return;
    }
    err = display_service_create_dummy_input_blocker_locked();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "restore dummy input blocker failed: %s", esp_err_to_name(err));
    }
    s_display.dummy_draw_suspended = false;
    ESP_LOGI(TAG, "dummy draw resumed");
    display_service_notify_session_resume(session);
}

void display_service_exclusive_raw_suspend_locked(void)
{
    display_service_dummy_draw_suspend_locked();
}

void display_service_exclusive_raw_resume_locked(void)
{
    display_service_dummy_draw_resume_locked();
}
