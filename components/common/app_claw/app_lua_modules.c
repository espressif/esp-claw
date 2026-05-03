/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_lua_modules.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#if CONFIG_APP_CLAW_LUA_MODULE_DELAY
#include "lua_module_delay.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_CAPABILITY
#include "lua_module_capability.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_ADC
#include "lua_module_adc.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DHT
#include "lua_module_dht.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_EVENT_PUBLISHER
#include "lua_module_event_publisher.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_GPIO
#include "lua_module_gpio.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_I2C
#include "lua_module_i2c.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LED_STRIP
#include "lua_module_led_strip.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_STORAGE
#include "lua_module_storage.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BUTTON
#include "lua_module_button.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_KNOB
#include "lua_module_knob.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_ESP_HEAP
#include "lua_module_esp_heap.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_SYSTEM
#include "lua_module_system.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BOARD_MANAGER
#include "lua_module_board_manager.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IMU
#include "lua_module_imu.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_MAGNETOMETER
#include "lua_module_magnetometer.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_ENVIRONMENTAL_SENSOR
#include "lua_module_environmental_sensor.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_TOUCH
#include "lua_module_touch.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IR
#include "lua_module_ir.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_MCPWM
#include "lua_module_mcpwm.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_UART
#include "lua_module_uart.h"
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_AUDIO && defined(CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT)
#include "lua_module_audio.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DISPLAY
#include "lua_module_display.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LCD
#include "lua_module_lcd.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LCD_TOUCH && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT)
#include "lua_module_lcd_touch.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_CAMERA && defined(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
#include "lua_module_camera.h"
#endif

static const char *TAG = "app_lua_modules";

typedef esp_err_t (*app_lua_module_register_fn)(const char *fatfs_base_path);

typedef struct {
    const char *module_id;
    const char *display_name;
    app_lua_module_register_fn reg;
} app_lua_module_entry_t;

static bool app_lua_modules_config_empty(const char *value)
{
    size_t i;

    if (!value) {
        return true;
    }

    for (i = 0; value[i]; i++) {
        if (!isspace((unsigned char)value[i])) {
            return false;
        }
    }

    return true;
}

static char *app_lua_trim_token(char *token)
{
    char *end;

    if (!token) {
        return NULL;
    }

    while (*token && isspace((unsigned char)*token)) {
        token++;
    }

    end = token + strlen(token);
    while (end > token && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return token;
}

static int app_lua_find_entry(const app_lua_module_entry_t *entries,
                              size_t count,
                              const char *module_id)
{
    size_t i;

    if (!entries || !module_id || !module_id[0]) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        if (entries[i].module_id && strcmp(entries[i].module_id, module_id) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static esp_err_t app_lua_build_module_map(const char *configured_modules,
                                          const app_lua_module_entry_t *entries,
                                          size_t entry_count,
                                          bool *selected_map)
{
    char *modules_copy = NULL;
    char *saveptr = NULL;
    char *token = NULL;
    size_t i;

    if (!entries || !selected_map) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(selected_map, 0, entry_count * sizeof(selected_map[0]));

    if (app_lua_modules_config_empty(configured_modules)) {
        for (i = 0; i < entry_count; i++) {
            selected_map[i] = true;
        }
        return ESP_OK;
    }

    modules_copy = malloc(strlen(configured_modules) + 1);
    if (!modules_copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(modules_copy, configured_modules, strlen(configured_modules) + 1);

    for (token = strtok_r(modules_copy, ",", &saveptr);
            token;
            token = strtok_r(NULL, ",", &saveptr)) {
        char *trimmed = app_lua_trim_token(token);
        int index;

        if (!trimmed || !trimmed[0]) {
            continue;
        }

        if (strcmp(trimmed, "__none__") == 0 || strcmp(trimmed, "none") == 0) {
            continue;
        }

        index = app_lua_find_entry(entries, entry_count, trimmed);
        if (index < 0) {
            ESP_LOGW(TAG, "Ignoring unknown or unavailable Lua module: %s", trimmed);
            continue;
        }

        selected_map[index] = true;
    }

    free(modules_copy);
    return ESP_OK;
}

#if CONFIG_APP_CLAW_LUA_MODULE_DELAY
static esp_err_t app_lua_register_delay(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_delay_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_CAPABILITY
static esp_err_t app_lua_register_capability(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_capability_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_ADC
static esp_err_t app_lua_register_adc(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_adc_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_DHT
static esp_err_t app_lua_register_dht(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_dht_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_EVENT_PUBLISHER
static esp_err_t app_lua_register_event_publisher(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_event_publisher_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_GPIO
static esp_err_t app_lua_register_gpio(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_gpio_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_I2C
static esp_err_t app_lua_register_i2c(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_i2c_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_LED_STRIP
static esp_err_t app_lua_register_led_strip(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_led_strip_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_STORAGE
static esp_err_t app_lua_register_storage(const char *fatfs_base_path)
{
    return lua_module_storage_register(fatfs_base_path);
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_BUTTON
static esp_err_t app_lua_register_button(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_button_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_KNOB
static esp_err_t app_lua_register_knob(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_knob_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_ESP_HEAP
static esp_err_t app_lua_register_esp_heap(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_esp_heap_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_SYSTEM
static esp_err_t app_lua_register_system(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_system_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_BOARD_MANAGER
static esp_err_t app_lua_register_board_manager(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_board_manager_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_IMU
static esp_err_t app_lua_register_imu(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_imu_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_MAGNETOMETER
static esp_err_t app_lua_register_magnetometer(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_magnetometer_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_ENVIRONMENTAL_SENSOR
static esp_err_t app_lua_register_environmental_sensor(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_environmental_sensor_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_TOUCH
static esp_err_t app_lua_register_touch(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_touch_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_IR
static esp_err_t app_lua_register_ir(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_ir_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_MCPWM
static esp_err_t app_lua_register_mcpwm(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_mcpwm_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_UART
static esp_err_t app_lua_register_uart(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_uart_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_AUDIO && defined(CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT)
static esp_err_t app_lua_register_audio(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_audio_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_DISPLAY
static esp_err_t app_lua_register_display(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_display_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_LCD
static esp_err_t app_lua_register_lcd(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_lcd_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_LCD_TOUCH && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT)
static esp_err_t app_lua_register_lcd_touch(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_lcd_touch_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_CAMERA && defined(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
static esp_err_t app_lua_register_camera(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_camera_register();
}
#endif

static const app_lua_module_entry_t s_lua_module_entries[] = {
#if CONFIG_APP_CLAW_LUA_MODULE_ADC
    { "adc", "ADC", app_lua_register_adc },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DHT
    { "dht", "DHT", app_lua_register_dht },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_CAPABILITY
    { "capability", "Capability", app_lua_register_capability },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DELAY
    { "delay", "Delay", app_lua_register_delay },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_STORAGE
    { "storage", "Storage", app_lua_register_storage },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_GPIO
    { "gpio", "GPIO", app_lua_register_gpio },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_I2C
    { "i2c", "I2C", app_lua_register_i2c },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LED_STRIP
    { "led_strip", "LED Strip", app_lua_register_led_strip },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_AUDIO && defined(CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT)
    { "audio", "Audio", app_lua_register_audio },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BUTTON
    { "button", "Button", app_lua_register_button },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_KNOB
    { "knob", "Knob", app_lua_register_knob },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DISPLAY
    { "display", "Display", app_lua_register_display },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LCD
    { "lcd", "LCD", app_lua_register_lcd },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BOARD_MANAGER
    { "board_manager", "Board Manager", app_lua_register_board_manager },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IMU
    { "imu", "IMU", app_lua_register_imu },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_MAGNETOMETER
    { "magnetometer", "Magnetometer", app_lua_register_magnetometer },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_ENVIRONMENTAL_SENSOR
    { "environmental_sensor", "Environmental Sensor", app_lua_register_environmental_sensor },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_TOUCH
    { "touch", "Touch", app_lua_register_touch },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IR
    { "ir", "IR", app_lua_register_ir },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LCD_TOUCH && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT)
    { "lcd_touch", "LCD Touch", app_lua_register_lcd_touch },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_CAMERA && defined(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
    { "camera", "Camera", app_lua_register_camera },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_ESP_HEAP
    { "esp_heap", "ESP Heap", app_lua_register_esp_heap },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_SYSTEM
    { "system", "System", app_lua_register_system },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_MCPWM
    { "mcpwm", "MCPWM", app_lua_register_mcpwm },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_UART
    { "uart", "UART", app_lua_register_uart },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_EVENT_PUBLISHER
    { "event_publisher", "Event Publisher", app_lua_register_event_publisher },
#endif
};

static const app_lua_module_info_t s_lua_module_infos[] = {
#if CONFIG_APP_CLAW_LUA_MODULE_ADC
    { "adc", "ADC" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DHT
    { "dht", "DHT" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_CAPABILITY
    { "capability", "Capability" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DELAY
    { "delay", "Delay" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_STORAGE
    { "storage", "Storage" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_GPIO
    { "gpio", "GPIO" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_I2C
    { "i2c", "I2C" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LED_STRIP
    { "led_strip", "LED Strip" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_AUDIO && defined(CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT)
    { "audio", "Audio" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BUTTON
    { "button", "Button" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_KNOB
    { "knob", "Knob" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DISPLAY
    { "display", "Display" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LCD
    { "lcd", "LCD" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BOARD_MANAGER
    { "board_manager", "Board Manager" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IMU
    { "imu", "IMU" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_MAGNETOMETER
    { "magnetometer", "Magnetometer" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_ENVIRONMENTAL_SENSOR
    { "environmental_sensor", "Environmental Sensor" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_TOUCH
    { "touch", "Touch" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IR
    { "ir", "IR" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LCD_TOUCH && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT)
    { "lcd_touch", "LCD Touch" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_CAMERA && defined(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
    { "camera", "Camera" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_ESP_HEAP
    { "esp_heap", "ESP Heap" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_SYSTEM
    { "system", "System" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_MCPWM
    { "mcpwm", "MCPWM" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_UART
    { "uart", "UART" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_EVENT_PUBLISHER
    { "event_publisher", "Event Publisher" },
#endif
};

esp_err_t app_lua_modules_register(const app_claw_config_t *config, const char *fatfs_base_path)
{
    const size_t entry_count = sizeof(s_lua_module_entries) / sizeof(s_lua_module_entries[0]);
    bool *selected_map = NULL;
    esp_err_t err = ESP_OK;
    size_t i;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    selected_map = calloc(entry_count > 0 ? entry_count : 1, sizeof(selected_map[0]));
    if (!selected_map) {
        return ESP_ERR_NO_MEM;
    }

    err = app_lua_build_module_map(config->enabled_lua_modules,
                                   s_lua_module_entries,
                                   entry_count,
                                   selected_map);
    if (err != ESP_OK) {
        free(selected_map);
        return err;
    }

    for (i = 0; i < entry_count; i++) {
        if (!selected_map[i]) {
            ESP_LOGI(TAG, "Skipping Lua module at init: %s", s_lua_module_entries[i].module_id);
            continue;
        }

        err = s_lua_module_entries[i].reg(fatfs_base_path);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register Lua module %s: %s",
                     s_lua_module_entries[i].module_id,
                     esp_err_to_name(err));
            free(selected_map);
            return err;
        }
    }

    free(selected_map);
    return ESP_OK;
}

esp_err_t app_lua_modules_get_compiled_modules(const app_lua_module_info_t **modules,
                                               size_t *count)
{
    if (!modules || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    *modules = s_lua_module_infos;
    *count = sizeof(s_lua_module_infos) / sizeof(s_lua_module_infos[0]);
    return ESP_OK;
}
