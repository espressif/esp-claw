/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ATK-DNESP32S3-BOX0 board-specific device factories.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gen_board_device_custom.h"

static const char *TAG = "atk_box0_setup";

typedef struct {
    esp_timer_handle_t timer;
    adc_oneshot_unit_handle_t adc;
    gpio_num_t charge_ctrl_gpio;
    gpio_num_t charge_status_gpio;
    gpio_num_t sys_power_gpio;
    int battery_adc_channel;
    int low_voltage_adc_threshold;
    bool shutdown_on_low_battery;
    bool adc_enabled;
} box0_power_manager_t;

static int box0_battery_level_from_adc(uint32_t adc_value)
{
    static const struct {
        uint16_t adc;
        uint8_t level;
    } levels[] = {
        {2951, 0},
        {3019, 20},
        {3037, 40},
        {3091, 60},
        {3124, 80},
        {3231, 100},
    };

    if (adc_value < levels[0].adc) {
        return 0;
    }
    if (adc_value >= levels[5].adc) {
        return 100;
    }

    for (size_t i = 0; i < 5; i++) {
        if (adc_value >= levels[i].adc && adc_value < levels[i + 1].adc) {
            const float ratio = (float)(adc_value - levels[i].adc) /
                                (float)(levels[i + 1].adc - levels[i].adc);
            return (int)(levels[i].level + ratio * (levels[i + 1].level - levels[i].level));
        }
    }
    return 0;
}

static esp_err_t box0_read_battery_adc(box0_power_manager_t *pm, int *out_adc)
{
    int total = 0;
    int value = 0;

    if (!pm || !pm->adc_enabled || !out_adc) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_set_level(pm->charge_ctrl_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    for (int i = 0; i < 10; i++) {
        ESP_RETURN_ON_ERROR(adc_oneshot_read(pm->adc, (adc_channel_t)pm->battery_adc_channel, &value),
                            TAG, "battery ADC read failed");
        total += value;
    }
    gpio_set_level(pm->charge_ctrl_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    *out_adc = total / 10;
    return ESP_OK;
}

static void box0_power_sample_timer_cb(void *arg)
{
    box0_power_manager_t *pm = (box0_power_manager_t *)arg;
    int adc = 0;
    bool charging;
    int level;

    if (!pm) {
        return;
    }

    charging = gpio_get_level(pm->charge_status_gpio) == 0;
    if (box0_read_battery_adc(pm, &adc) != ESP_OK) {
        ESP_LOGW(TAG, "battery sample failed, charging=%d", charging);
        return;
    }

    level = box0_battery_level_from_adc((uint32_t)adc);
    ESP_LOGI(TAG, "battery adc=%d level=%d%% charging=%d", adc, level, charging);

    if (!charging && pm->shutdown_on_low_battery && adc < pm->low_voltage_adc_threshold) {
        ESP_LOGW(TAG, "battery below threshold (%d < %d), powering off", adc, pm->low_voltage_adc_threshold);
        gpio_set_level(pm->charge_ctrl_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(pm->sys_power_gpio, 0);
    }
}

static esp_err_t box0_power_manager_init(void *config, int cfg_size, void **device_handle)
{
    const dev_custom_box0_power_manager_config_t *cfg =
        (const dev_custom_box0_power_manager_config_t *)config;
    box0_power_manager_t *pm;
    gpio_config_t io_conf = {0};
    adc_oneshot_unit_init_cfg_t adc_unit_cfg = {0};
    adc_oneshot_chan_cfg_t adc_chan_cfg = {0};
    esp_timer_create_args_t timer_args = {0};
    uint32_t sample_period_ms;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(cfg && device_handle, ESP_ERR_INVALID_ARG, TAG, "invalid power manager args");
    ESP_RETURN_ON_FALSE(cfg_size == sizeof(*cfg), ESP_ERR_INVALID_SIZE, TAG, "unexpected power manager config size");

    pm = calloc(1, sizeof(*pm));
    ESP_RETURN_ON_FALSE(pm, ESP_ERR_NO_MEM, TAG, "failed to allocate power manager");

    pm->charge_ctrl_gpio = (gpio_num_t)cfg->charge_ctrl_gpio;
    pm->charge_status_gpio = (gpio_num_t)cfg->charge_status_gpio;
    pm->sys_power_gpio = (gpio_num_t)cfg->sys_power_gpio;
    pm->battery_adc_channel = cfg->battery_adc_channel;
    pm->low_voltage_adc_threshold = cfg->low_voltage_adc_threshold;
    pm->shutdown_on_low_battery = cfg->shutdown_on_low_battery;

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << cfg->sys_power_gpio) |
                           (1ULL << cfg->codec_power_gpio) |
                           (1ULL << cfg->charge_ctrl_gpio);
    ESP_GOTO_ON_ERROR(gpio_config(&io_conf), fail, TAG, "configure power GPIOs failed");
    gpio_set_level((gpio_num_t)cfg->sys_power_gpio, 1);
    gpio_set_level((gpio_num_t)cfg->codec_power_gpio, 1);
    gpio_set_level((gpio_num_t)cfg->charge_ctrl_gpio, 1);

    io_conf = (gpio_config_t) {
        .pin_bit_mask = (1ULL << cfg->charge_status_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&io_conf), fail, TAG, "configure charge status GPIO failed");

    adc_unit_cfg.unit_id = (adc_unit_t)cfg->battery_adc_unit;
    ESP_GOTO_ON_ERROR(adc_oneshot_new_unit(&adc_unit_cfg, &pm->adc), fail, TAG, "create ADC unit failed");
    pm->adc_enabled = true;

    adc_chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_chan_cfg.atten = (adc_atten_t)cfg->battery_adc_atten;
    ESP_GOTO_ON_ERROR(adc_oneshot_config_channel(pm->adc, (adc_channel_t)cfg->battery_adc_channel, &adc_chan_cfg),
                      fail, TAG, "configure battery ADC channel failed");

    timer_args.callback = box0_power_sample_timer_cb;
    timer_args.arg = pm;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "box0_power";
    timer_args.skip_unhandled_events = true;
    ESP_GOTO_ON_ERROR(esp_timer_create(&timer_args, &pm->timer), fail, TAG, "create power timer failed");

    sample_period_ms = cfg->sample_period_ms > 0 ? (uint32_t)cfg->sample_period_ms : 300000;
    ESP_GOTO_ON_ERROR(esp_timer_start_periodic(pm->timer, sample_period_ms * 1000ULL),
                      fail, TAG, "start power timer failed");

    box0_power_sample_timer_cb(pm);
    *device_handle = pm;
    ESP_LOGI(TAG, "BOX0 power manager initialized");
    return ESP_OK;

fail:
    if (pm) {
        if (pm->timer) {
            esp_timer_delete(pm->timer);
        }
        if (pm->adc_enabled) {
            adc_oneshot_del_unit(pm->adc);
        }
        free(pm);
    }
    return ESP_FAIL;
}

static int box0_power_manager_deinit(void *device_handle)
{
    box0_power_manager_t *pm = (box0_power_manager_t *)device_handle;

    if (!pm) {
        return ESP_OK;
    }
    if (pm->timer) {
        esp_timer_stop(pm->timer);
        esp_timer_delete(pm->timer);
    }
    if (pm->adc_enabled) {
        adc_oneshot_del_unit(pm->adc);
    }
    free(pm);
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(box0_power_manager, box0_power_manager_init, box0_power_manager_deinit);

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};

    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));
    esp_err_t ret = esp_lcd_new_panel_st7789(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
