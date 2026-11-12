/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NullPod power-key supervisor.
 *
 * The NullPod is a battery handheld with a hardware power latch: the PMIC
 * keeps the board powered only while PWR_HOLD (GPIO18) is driven high. The
 * power key (GPIO17, active low) is a plain GPIO with no PMIC-side long-press
 * logic, so shutdown is implemented in software:
 *
 *   - a poll task watches the key level,
 *   - after `long_press_ms` of continuous press the task drops PWR_HOLD, which
 *     releases the latch and switches the board off,
 *   - if the rail is still up afterwards (e.g. powered over USB) the task
 *     falls back to deep sleep so the device still behaves like it is off.
 *
 * The 2 s long-press threshold follows the stock NullPod firmware launcher
 * (see the long-press handler in NullPod_Master/src/launcher.cpp).
 */
#include <stdlib.h>
#include <string.h>

#include "dev_custom.h"
#include "esp_board_device.h"
#include "esp_board_entry.h"
#include "esp_board_periph.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gen_board_device_custom.h"

static const char *TAG = "NULLPOD_PWRKEY";

/* Require two consecutive samples before treating the level as stable. */
#define NULLPOD_PWRKEY_DEBOUNCE_SAMPLES   2
/* Guard against an accidental instant shutdown from a bogus config value. */
#define NULLPOD_PWRKEY_MIN_LONG_PRESS_MS  500
/* Let the log line flush before the rail drops. */
#define NULLPOD_PWRKEY_POWER_OFF_DELAY_MS 500
/* Give the latch time to discharge before the deep-sleep fallback. */
#define NULLPOD_PWRKEY_LATCH_SETTLE_MS    1000
#define NULLPOD_PWRKEY_TASK_STACK_SIZE    3072
#define NULLPOD_PWRKEY_TASK_PRIORITY      3
#define NULLPOD_PWRKEY_DEFAULT_POLL_MS    20

typedef struct {
    uint8_t      key_gpio;
    uint8_t      hold_gpio;
    uint16_t     long_press_ms;
    uint8_t      poll_interval_ms;
    TaskHandle_t task_handle;
    bool         running;
} nullpod_pwrkey_t;

static nullpod_pwrkey_t *s_pwrkey;

static bool nullpod_pwrkey_is_pressed(const nullpod_pwrkey_t *pk)
{
    /* The key is wired active-low with an external pull-up. */
    return gpio_get_level((gpio_num_t)pk->key_gpio) == 0;
}

static void nullpod_pwrkey_power_off(nullpod_pwrkey_t *pk)
{
    ESP_LOGW(TAG, "Power key long-pressed (%u ms): releasing power latch (GPIO%u)",
             (unsigned)pk->long_press_ms, (unsigned)pk->hold_gpio);

    vTaskDelay(pdMS_TO_TICKS(NULLPOD_PWRKEY_POWER_OFF_DELAY_MS));
    gpio_set_level((gpio_num_t)pk->hold_gpio, 0);

    /* Normally the board dies here. If the rail is still up (for example when
     * powered over USB), deep sleep keeps PWR_HOLD low so the device still
     * behaves like it is switched off. */
    vTaskDelay(pdMS_TO_TICKS(NULLPOD_PWRKEY_LATCH_SETTLE_MS));
    esp_deep_sleep_start();
}

static void nullpod_pwrkey_task(void *arg)
{
    nullpod_pwrkey_t *pk = (nullpod_pwrkey_t *)arg;
    bool pressed = false;
    uint32_t pressed_ms = 0;
    uint8_t low_samples = 0;
    uint8_t high_samples = 0;

    /* The key that powered the board on is normally still held when this task
     * starts. Ignore that press until it is released, otherwise booting would
     * immediately trigger a shutdown. */
    while (pk->running && nullpod_pwrkey_is_pressed(pk)) {
        vTaskDelay(pdMS_TO_TICKS(pk->poll_interval_ms));
    }

    while (pk->running) {
        vTaskDelay(pdMS_TO_TICKS(pk->poll_interval_ms));

        if (nullpod_pwrkey_is_pressed(pk)) {
            high_samples = 0;
            if (low_samples < NULLPOD_PWRKEY_DEBOUNCE_SAMPLES) {
                low_samples++;
            }
        } else {
            low_samples = 0;
            if (high_samples < NULLPOD_PWRKEY_DEBOUNCE_SAMPLES) {
                high_samples++;
            }
        }

        if (!pressed && low_samples >= NULLPOD_PWRKEY_DEBOUNCE_SAMPLES) {
            pressed = true;
            pressed_ms = 0;
        } else if (pressed && high_samples >= NULLPOD_PWRKEY_DEBOUNCE_SAMPLES) {
            pressed = false;
            pressed_ms = 0;
        }

        if (pressed) {
            pressed_ms += pk->poll_interval_ms;
            if (pressed_ms >= pk->long_press_ms) {
                nullpod_pwrkey_power_off(pk);
            }
        }
    }

    vTaskDelete(NULL);
}

static int nullpod_pwrkey_init(void *config, int cfg_size, void **device_handle)
{
    if (config == NULL || device_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg_size != (int)sizeof(dev_custom_power_key_config_t)) {
        ESP_LOGE(TAG, "Invalid config size: %d, expected: %d",
                 cfg_size, (int)sizeof(dev_custom_power_key_config_t));
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_pwrkey != NULL) {
        *device_handle = s_pwrkey;
        return ESP_OK;
    }

    const dev_custom_power_key_config_t *cfg = (const dev_custom_power_key_config_t *)config;

    if (cfg->chip == NULL || strcmp(cfg->chip, "nullpod_pwr") != 0) {
        ESP_LOGE(TAG, "Unsupported power_key chip: %s", cfg->chip ? cfg->chip : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    nullpod_pwrkey_t *pk = calloc(1, sizeof(*pk));
    if (pk == NULL) {
        return ESP_ERR_NO_MEM;
    }

    pk->key_gpio = cfg->key_gpio_num;
    pk->hold_gpio = cfg->hold_gpio_num;
    pk->long_press_ms = cfg->long_press_ms < NULLPOD_PWRKEY_MIN_LONG_PRESS_MS
                            ? NULLPOD_PWRKEY_MIN_LONG_PRESS_MS
                            : cfg->long_press_ms;
    pk->poll_interval_ms = cfg->poll_interval_ms == 0
                               ? NULLPOD_PWRKEY_DEFAULT_POLL_MS
                               : cfg->poll_interval_ms;
    pk->running = true;

    /* The key pin is already configured as input+pull-up by the board manager
     * (gpio_pwr_key peripheral); repeat it here so the device stays correct
     * even if that peripheral setup changes. The hold pin stays owned by the
     * gpio_pwr_hold peripheral — only its level is touched at power-off. */
    gpio_reset_pin((gpio_num_t)pk->key_gpio);
    gpio_set_direction((gpio_num_t)pk->key_gpio, GPIO_MODE_INPUT);
    gpio_pullup_en((gpio_num_t)pk->key_gpio);

    BaseType_t ok = xTaskCreate(nullpod_pwrkey_task, "nullpod_pwrkey",
                                NULLPOD_PWRKEY_TASK_STACK_SIZE, pk,
                                NULLPOD_PWRKEY_TASK_PRIORITY, &pk->task_handle);
    if (ok != pdPASS) {
        free(pk);
        ESP_LOGE(TAG, "Failed to create power key monitor task");
        return ESP_ERR_NO_MEM;
    }

    s_pwrkey = pk;
    *device_handle = pk;
    ESP_LOGI(TAG, "Power key monitor started: key=GPIO%u hold=GPIO%u long_press=%u ms",
             (unsigned)pk->key_gpio, (unsigned)pk->hold_gpio, (unsigned)pk->long_press_ms);
    return ESP_OK;
}

static int nullpod_pwrkey_deinit(void *device_handle)
{
    nullpod_pwrkey_t *pk = (nullpod_pwrkey_t *)device_handle;
    if (pk == NULL) {
        return ESP_OK;
    }

    pk->running = false;
    if (pk->task_handle != NULL) {
        vTaskDelete(pk->task_handle);
        pk->task_handle = NULL;
    }
    if (s_pwrkey == pk) {
        s_pwrkey = NULL;
    }
    free(pk);
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(power_key, nullpod_pwrkey_init, nullpod_pwrkey_deinit);
