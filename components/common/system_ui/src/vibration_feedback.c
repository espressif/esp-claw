/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "system_ui_private.h"
#include "esp_board_manager.h"
#include "esp_board_manager_includes.h"

#if defined(CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT)

#include "esp_check.h"
#include "esp_log.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#define TAG "ui_vibration"
#define SYSTEM_UI_VIBRATION_DEVICE_NAME "vibration_motor"
#define APP_CLAW_VIBRATION_PULSE_MS 25
#define APP_CLAW_VIBRATION_MIN_INTERVAL_MS 80

static TimerHandle_t s_vibration_timer;
static TickType_t s_last_pulse_tick;
static bool s_vibration_ready;
static uint32_t s_vibration_active_duty;

static TickType_t system_ui_vibration_ms_to_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);

    return ticks > 0 ? ticks : 1;
}

static esp_err_t system_ui_vibration_set_enabled(bool enabled)
{
    periph_ledc_handle_t *ledc_handle = NULL;

    ESP_RETURN_ON_FALSE(s_vibration_ready, ESP_ERR_INVALID_STATE, TAG, "vibration LEDC is not ready");
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(SYSTEM_UI_VIBRATION_DEVICE_NAME, (void **)&ledc_handle),
                        TAG, "get vibration motor LEDC handle failed");
    ESP_RETURN_ON_FALSE(ledc_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "vibration LEDC handle is NULL");

    // Match the ESP-IDF LEDC flow: set duty first, then update duty to apply it.
    ESP_RETURN_ON_ERROR(ledc_set_duty(ledc_handle->speed_mode, ledc_handle->channel, enabled ? s_vibration_active_duty : 0),
                        TAG, "set vibration motor duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(ledc_handle->speed_mode, ledc_handle->channel),
                        TAG, "update vibration motor duty failed");
    return ESP_OK;
}

static void system_ui_vibration_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    esp_err_t err = system_ui_vibration_set_enabled(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stop vibration motor failed: %s", esp_err_to_name(err));
    }
}

static void system_ui_vibration_click_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        system_ui_click_feedback();
    }
}

esp_err_t system_ui_vibration_init(void)
{
    esp_err_t ret = ESP_OK;
    dev_ledc_ctrl_config_t *device_config = NULL;
    periph_ledc_config_t *periph_config = NULL;
    periph_ledc_handle_t *ledc_handle = NULL;

    if (s_vibration_timer != NULL) {
        return ESP_OK;
    }
    if (!esp_board_manager_check_name(SYSTEM_UI_VIBRATION_DEVICE_NAME)) {
        ESP_LOGI(TAG, "board manager device '%s' not found, skip vibration", SYSTEM_UI_VIBRATION_DEVICE_NAME);
        return ESP_OK;
    }

    // Use the already initialized Board Manager LEDC device; system_ui only changes its duty.
    ESP_GOTO_ON_ERROR(esp_board_manager_get_device_handle(SYSTEM_UI_VIBRATION_DEVICE_NAME, (void **)&ledc_handle),
                      fail, TAG, "get vibration motor LEDC handle failed");
    ESP_GOTO_ON_ERROR(esp_board_manager_get_device_config(SYSTEM_UI_VIBRATION_DEVICE_NAME, (void **)&device_config),
                      fail, TAG, "get vibration motor LEDC config failed");
    ESP_GOTO_ON_FALSE(device_config != NULL && device_config->ledc_name != NULL && device_config->default_percent <= 100,
                      ESP_ERR_INVALID_ARG, fail, TAG, "invalid vibration motor LEDC config");
    ESP_GOTO_ON_ERROR(esp_board_manager_get_periph_config(device_config->ledc_name, (void **)&periph_config),
                      fail, TAG, "get vibration motor LEDC peripheral config failed");
    ESP_GOTO_ON_FALSE(ledc_handle != NULL && periph_config != NULL && periph_config->duty_resolution < 31,
                      ESP_ERR_INVALID_ARG, fail, TAG, "invalid vibration motor LEDC peripheral config");

    s_vibration_active_duty = (device_config->default_percent * ((1U << (uint32_t)periph_config->duty_resolution) - 1U)) / 100U;
    s_vibration_ready = true;
    ESP_GOTO_ON_ERROR(system_ui_vibration_set_enabled(false), fail, TAG, "disable vibration motor failed");

    s_vibration_timer = xTimerCreate("ui_vibration", system_ui_vibration_ms_to_ticks(APP_CLAW_VIBRATION_PULSE_MS), pdFALSE, NULL, system_ui_vibration_timer_cb);
    if (s_vibration_timer == NULL) {
        ESP_LOGE(TAG, "create vibration timer failed");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_last_pulse_tick = 0;
    ESP_LOGI(TAG, "vibration motor enabled: %s", SYSTEM_UI_VIBRATION_DEVICE_NAME);
    return ESP_OK;

fail:
    s_vibration_ready = false;
    s_vibration_active_duty = 0;
    return ret;
}

void system_ui_vibration_deinit(void)
{
    if (s_vibration_timer != NULL) {
        (void)xTimerStop(s_vibration_timer, portMAX_DELAY);
        (void)xTimerDelete(s_vibration_timer, portMAX_DELAY);
        s_vibration_timer = NULL;
    }

    if (s_vibration_ready) {
        esp_err_t err = system_ui_vibration_set_enabled(false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "disable vibration motor during deinit failed: %s", esp_err_to_name(err));
        }
    }
    s_vibration_ready = false;
    s_vibration_active_duty = 0;
    s_last_pulse_tick = 0;
}

void system_ui_click_feedback(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t min_interval = system_ui_vibration_ms_to_ticks(APP_CLAW_VIBRATION_MIN_INTERVAL_MS);

    if (s_vibration_timer == NULL) {
        return;
    }
    if (s_last_pulse_tick != 0 && now - s_last_pulse_tick < min_interval) {
        return;
    }
    s_last_pulse_tick = now;

    // Keep the motor active only until the one-shot timer switches it off.
    esp_err_t err = system_ui_vibration_set_enabled(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start vibration motor failed: %s", esp_err_to_name(err));
        return;
    }
    (void)xTimerStop(s_vibration_timer, 0);
    if (xTimerChangePeriod(s_vibration_timer, system_ui_vibration_ms_to_ticks(APP_CLAW_VIBRATION_PULSE_MS), 0) != pdPASS) {
        ESP_LOGE(TAG, "start vibration timer failed");
        (void)system_ui_vibration_set_enabled(false);
    }
}

void system_ui_add_click_feedback(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_add_event_cb(obj, system_ui_vibration_click_event_cb, LV_EVENT_CLICKED, NULL);
}

#else

esp_err_t system_ui_vibration_init(void)
{
    return ESP_OK;
}

void system_ui_vibration_deinit(void)
{
}

void system_ui_click_feedback(void)
{
}

void system_ui_add_click_feedback(lv_obj_t *obj)
{
    (void)obj;
}

#endif
