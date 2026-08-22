/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "system_ui_private.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_board_manager.h"
#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#define TAG "ui_vibration"
#define SYSTEM_UI_VIBRATION_DEVICE_NAME "vibration_motor"
#define SYSTEM_UI_VIBRATION_DEVICE_TYPE "custom"
#define SYSTEM_UI_VIBRATION_ACTIVE_LEVEL 1
#define SYSTEM_UI_VIBRATION_INACTIVE_LEVEL 0
#define APP_CLAW_VIBRATION_PULSE_MS 25
#define APP_CLAW_VIBRATION_MIN_INTERVAL_MS 80

typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t gpio_num;
} system_ui_vibration_config_t;

static TimerHandle_t s_vibration_timer;
static TickType_t s_last_pulse_tick;
static const char *s_vibration_device_name;
static gpio_num_t s_vibration_gpio = GPIO_NUM_NC;

static TickType_t system_ui_vibration_ms_to_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);

    return ticks > 0 ? ticks : 1;
}

static esp_err_t system_ui_vibration_set_enabled(bool enabled)
{
    if (s_vibration_gpio == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_STATE;
    }
    return gpio_set_level(s_vibration_gpio, enabled ? SYSTEM_UI_VIBRATION_ACTIVE_LEVEL : SYSTEM_UI_VIBRATION_INACTIVE_LEVEL);
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

static esp_err_t system_ui_vibration_resolve_device(void)
{
    if (esp_board_manager_check_name(SYSTEM_UI_VIBRATION_DEVICE_NAME)) {
        s_vibration_device_name = SYSTEM_UI_VIBRATION_DEVICE_NAME;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t system_ui_vibration_load_config(const char *device_name)
{
    void *config = NULL;
    system_ui_vibration_config_t *vibration_config = NULL;

    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config(device_name, &config), TAG, "get vibration device config failed");
    vibration_config = (system_ui_vibration_config_t *)config;
    ESP_RETURN_ON_FALSE(vibration_config != NULL && vibration_config->type != NULL && strcmp(vibration_config->type, SYSTEM_UI_VIBRATION_DEVICE_TYPE) == 0,
                        ESP_ERR_INVALID_ARG, TAG, "vibration device is not a custom device");
    ESP_RETURN_ON_FALSE(vibration_config->gpio_num >= 0 && vibration_config->gpio_num < GPIO_NUM_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid vibration gpio: %d", (int)vibration_config->gpio_num);
    s_vibration_gpio = (gpio_num_t)vibration_config->gpio_num;
    return ESP_OK;
}

esp_err_t system_ui_vibration_init(void)
{
    esp_err_t err = ESP_OK;

    if (s_vibration_timer != NULL) {
        return ESP_OK;
    }

    err = system_ui_vibration_resolve_device();
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "board manager device '%s' not found, skip vibration", SYSTEM_UI_VIBRATION_DEVICE_NAME);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "resolve vibration motor device failed");

    ESP_RETURN_ON_ERROR(system_ui_vibration_load_config(s_vibration_device_name), TAG, "load vibration motor config failed");
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(s_vibration_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "vibration gpio config failed");
    err = system_ui_vibration_set_enabled(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "disable vibration motor failed: %s", esp_err_to_name(err));
        goto fail;
    }

    s_vibration_timer = xTimerCreate("ui_vibration", system_ui_vibration_ms_to_ticks(APP_CLAW_VIBRATION_PULSE_MS), pdFALSE, NULL, system_ui_vibration_timer_cb);
    if (s_vibration_timer == NULL) {
        ESP_LOGE(TAG, "create vibration timer failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_last_pulse_tick = 0;
    ESP_LOGI(TAG, "vibration motor enabled: %s", s_vibration_device_name);
    return ESP_OK;

fail:
    s_vibration_device_name = NULL;
    s_vibration_gpio = GPIO_NUM_NC;
    return err;
}

void system_ui_vibration_deinit(void)
{
    if (s_vibration_timer != NULL) {
        (void)xTimerStop(s_vibration_timer, portMAX_DELAY);
        (void)xTimerDelete(s_vibration_timer, portMAX_DELAY);
        s_vibration_timer = NULL;
    }

    if (s_vibration_device_name != NULL) {
        esp_err_t err = system_ui_vibration_set_enabled(false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "disable vibration motor during deinit failed: %s", esp_err_to_name(err));
        }
    }
    s_vibration_device_name = NULL;
    s_vibration_gpio = GPIO_NUM_NC;
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
