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

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

/* --- lua_driver (hardware peripheral drivers) --- */
#if CONFIG_APP_CLAW_LUA_DRIVER_ADC
#include "lua_driver_adc.h"
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_GPIO
#include "lua_driver_gpio.h"
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_I2C
#include "lua_driver_i2c.h"
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_MCPWM
#include "lua_driver_mcpwm.h"
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_PCNT
#include "lua_driver_pcnt.h"
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_RMT
#include "lua_driver_rmt.h"
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_TOUCH
#include "lua_driver_touch.h"
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_UART
#include "lua_driver_uart.h"
#endif

/* --- lua_module (higher-level modules) --- */
#if CONFIG_APP_CLAW_LUA_MODULE_AUDIO && defined(CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT)
#include "lua_module_audio.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BOARD_MANAGER
#include "lua_module_board_manager.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BUTTON
#include "lua_module_button.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BLE
#include "lua_module_ble.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BLE_HID
#include "lua_module_ble_hid.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_CAMERA && defined(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
#include "lua_module_camera.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_CAPABILITY
#include "lua_module_capability.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DELAY
#include "lua_module_delay.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DISPLAY
#include "lua_module_display.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_ENVIRONMENTAL_SENSOR
#include "lua_module_environmental_sensor.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_EVENT_PUBLISHER
#include "lua_module_event_publisher.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER
#include "lua_module_http_server.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_JSON
#include "lua_module_json.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IMAGE
#include "lua_image.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_THREAD
#include "lua_module_thread.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IMU
#include "lua_module_imu.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IR
#include "lua_module_ir.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_KNOB
#include "lua_module_knob.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LCD
#include "lua_module_lcd.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LCD_TOUCH && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT) && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUB_I2C_SUPPORT)
#include "lua_module_lcd_touch.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LEDC
#include "lua_module_ledc.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LED_STRIP
#include "lua_module_led_strip.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LVGL
#include "lua_module_lvgl.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_MAGNETOMETER
#include "lua_module_magnetometer.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_SCI
#include "lua_module_sci.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_STORAGE
#include "lua_module_storage.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_SYSTEM
#include "lua_module_system.h"
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_VISION && defined(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
#include "lua_module_vision.h"
#endif

static const char *TAG = "app_lua_modules";

#define APP_LUA_EXTERNAL_MODULE_INITIAL_CAPACITY 4

typedef struct {
    const char *module_id;
    const char *display_name;
    app_lua_module_register_fn reg;
} app_lua_module_entry_t;

static app_lua_module_external_t *s_external_modules;
static size_t s_external_module_count;
static size_t s_external_module_capacity;
static app_lua_module_info_t *s_module_infos;
static size_t s_module_info_capacity;

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

static int app_lua_find_external_module(const char *module_id)
{
    size_t i;

    if (!module_id || !module_id[0]) {
        return -1;
    }

    for (i = 0; i < s_external_module_count; i++) {
        if (s_external_modules[i].module_id &&
                strcmp(s_external_modules[i].module_id, module_id) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static esp_err_t app_lua_reserve_external_modules(size_t required_count)
{
    app_lua_module_external_t *new_modules = NULL;
    size_t new_capacity = s_external_module_capacity;

    if (required_count <= s_external_module_capacity) {
        return ESP_OK;
    }

    if (new_capacity == 0) {
        new_capacity = APP_LUA_EXTERNAL_MODULE_INITIAL_CAPACITY;
    }
    while (new_capacity < required_count) {
        new_capacity *= 2;
    }

    new_modules = realloc(s_external_modules, new_capacity * sizeof(new_modules[0]));
    if (!new_modules) {
        return ESP_ERR_NO_MEM;
    }

    s_external_modules = new_modules;
    s_external_module_capacity = new_capacity;
    return ESP_OK;
}

static esp_err_t app_lua_reserve_module_infos(size_t required_count)
{
    app_lua_module_info_t *new_infos = NULL;

    if (required_count <= s_module_info_capacity) {
        return ESP_OK;
    }

    new_infos = realloc(s_module_infos, required_count * sizeof(new_infos[0]));
    if (!new_infos) {
        return ESP_ERR_NO_MEM;
    }

    s_module_infos = new_infos;
    s_module_info_capacity = required_count;
    return ESP_OK;
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

/* --- lua_driver register wrappers --- */

#if CONFIG_APP_CLAW_LUA_DRIVER_ADC
static esp_err_t app_lua_register_adc(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_driver_adc_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_DRIVER_GPIO
static esp_err_t app_lua_register_gpio(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_driver_gpio_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_DRIVER_I2C
static esp_err_t app_lua_register_i2c(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_driver_i2c_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_DRIVER_MCPWM
static esp_err_t app_lua_register_mcpwm(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_driver_mcpwm_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_DRIVER_PCNT
static esp_err_t app_lua_register_pcnt(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_driver_pcnt_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_DRIVER_RMT
static esp_err_t app_lua_register_rmt(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_driver_rmt_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_DRIVER_TOUCH
static esp_err_t app_lua_register_touch(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_driver_touch_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_DRIVER_UART
static esp_err_t app_lua_register_uart(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_driver_uart_register();
}
#endif

/* --- lua_module register wrappers --- */

#if CONFIG_APP_CLAW_LUA_MODULE_AUDIO && defined(CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT)
static esp_err_t app_lua_register_audio(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_audio_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_BOARD_MANAGER
static esp_err_t app_lua_register_board_manager(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_board_manager_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_BUTTON
static esp_err_t app_lua_register_button(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_button_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_BLE
static esp_err_t app_lua_register_ble(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_ble_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_BLE_HID
static esp_err_t app_lua_register_ble_hid(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_ble_hid_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_CAMERA && defined(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
static esp_err_t app_lua_register_camera(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_camera_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_CAPABILITY
static esp_err_t app_lua_register_capability(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_capability_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_DELAY
static esp_err_t app_lua_register_delay(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_delay_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_DISPLAY
static esp_err_t app_lua_register_display(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_display_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_ENVIRONMENTAL_SENSOR
static esp_err_t app_lua_register_environmental_sensor(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_environmental_sensor_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_EVENT_PUBLISHER
static esp_err_t app_lua_register_event_publisher(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_event_publisher_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER
static esp_err_t app_lua_register_http_server(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_http_server_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_JSON
static esp_err_t app_lua_register_json(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_json_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_IMAGE
static esp_err_t app_lua_register_image(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_image_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_THREAD
static esp_err_t app_lua_register_thread(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_thread_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_IMU
static esp_err_t app_lua_register_imu(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_imu_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_IR
static esp_err_t app_lua_register_ir(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_ir_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_KNOB
static esp_err_t app_lua_register_knob(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_knob_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_LCD
static esp_err_t app_lua_register_lcd(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_lcd_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_LCD_TOUCH && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT) && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUB_I2C_SUPPORT)
static esp_err_t app_lua_register_lcd_touch(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_lcd_touch_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_LEDC
static esp_err_t app_lua_register_ledc(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_ledc_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_LED_STRIP
static esp_err_t app_lua_register_led_strip(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_led_strip_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_LVGL
static esp_err_t app_lua_register_lvgl(const char *fatfs_base_path)
{
    return lua_module_lvgl_register_with_data_root(fatfs_base_path);
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_MAGNETOMETER
static esp_err_t app_lua_register_magnetometer(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_magnetometer_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_SCI
static esp_err_t app_lua_register_sci(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_sci_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_STORAGE
static esp_err_t app_lua_register_storage(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_storage_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_SYSTEM
static esp_err_t app_lua_register_system(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_system_register();
}
#endif

#if CONFIG_APP_CLAW_LUA_MODULE_VISION && defined(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
static esp_err_t app_lua_register_vision(const char *fatfs_base_path)
{
    (void)fatfs_base_path;
    return lua_module_vision_register();
}
#endif

static const app_lua_module_entry_t s_lua_module_entries[] = {
    /* --- lua_driver (hardware peripheral drivers) --- */
#if CONFIG_APP_CLAW_LUA_DRIVER_ADC
    { "adc", "ADC", app_lua_register_adc },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_GPIO
    { "gpio", "GPIO", app_lua_register_gpio },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_I2C
    { "i2c", "I2C", app_lua_register_i2c },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_MCPWM
    { "mcpwm", "MCPWM", app_lua_register_mcpwm },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_PCNT
    { "pcnt", "PCNT", app_lua_register_pcnt },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_RMT
    { "rmt", "RMT", app_lua_register_rmt },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_TOUCH
    { "touch", "Touch", app_lua_register_touch },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_UART
    { "uart", "UART", app_lua_register_uart },
#endif
    /* --- lua_module (higher-level modules) --- */
#if CONFIG_APP_CLAW_LUA_MODULE_AUDIO && defined(CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT)
    { "audio", "Audio", app_lua_register_audio },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BOARD_MANAGER
    { "board_manager", "Board Manager", app_lua_register_board_manager },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BUTTON
    { "button", "Button", app_lua_register_button },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BLE
    { "ble", "BLE", app_lua_register_ble },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BLE_HID
    { "ble_hid", "BLE HID", app_lua_register_ble_hid },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_CAMERA && defined(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
    { "camera", "Camera", app_lua_register_camera },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_CAPABILITY
    { "capability", "Capability", app_lua_register_capability },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DELAY
    { "delay", "Delay", app_lua_register_delay },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DISPLAY
    { "display", "Display", app_lua_register_display },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_ENVIRONMENTAL_SENSOR
    { "environmental_sensor", "Environmental Sensor", app_lua_register_environmental_sensor },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_EVENT_PUBLISHER
    { "event_publisher", "Event Publisher", app_lua_register_event_publisher },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER
    { "http_server", "HTTP Server", app_lua_register_http_server },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_JSON
    { "json", "JSON", app_lua_register_json },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IMAGE
    { "image", "Image", app_lua_register_image },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_THREAD
    { "thread", "Thread", app_lua_register_thread },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IMU
    { "imu", "IMU", app_lua_register_imu },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IR
    { "ir", "IR", app_lua_register_ir },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_KNOB
    { "knob", "Knob", app_lua_register_knob },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LCD
    { "lcd", "LCD", app_lua_register_lcd },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LCD_TOUCH && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT) && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUB_I2C_SUPPORT)
    { "lcd_touch", "LCD Touch", app_lua_register_lcd_touch },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LEDC
    { "ledc", "LEDC", app_lua_register_ledc },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LED_STRIP
    { "led_strip", "LED Strip", app_lua_register_led_strip },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LVGL
    { "lvgl", "LVGL", app_lua_register_lvgl },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_MAGNETOMETER
    { "magnetometer", "Magnetometer", app_lua_register_magnetometer },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_SCI
    { "sci", "DFRobot SCI", app_lua_register_sci },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_STORAGE
    { "storage", "Storage", app_lua_register_storage },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_SYSTEM
    { "system", "System", app_lua_register_system },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_VISION && defined(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
    { "vision", "Vision", app_lua_register_vision },
#endif
};

static const app_lua_module_info_t s_lua_module_infos[] = {
    /* --- lua_driver (hardware peripheral drivers) --- */
#if CONFIG_APP_CLAW_LUA_DRIVER_ADC
    { "adc", "ADC" },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_GPIO
    { "gpio", "GPIO" },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_I2C
    { "i2c", "I2C" },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_MCPWM
    { "mcpwm", "MCPWM" },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_PCNT
    { "pcnt", "PCNT" },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_RMT
    { "rmt", "RMT" },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_TOUCH
    { "touch", "Touch" },
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_UART
    { "uart", "UART" },
#endif
    /* --- lua_module (higher-level modules) --- */
#if CONFIG_APP_CLAW_LUA_MODULE_AUDIO && defined(CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT)
    { "audio", "Audio" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BOARD_MANAGER
    { "board_manager", "Board Manager" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BUTTON
    { "button", "Button" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BLE
    { "ble", "BLE" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_BLE_HID
    { "ble_hid", "BLE HID" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_CAMERA && defined(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
    { "camera", "Camera" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_CAPABILITY
    { "capability", "Capability" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DELAY
    { "delay", "Delay" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_DISPLAY
    { "display", "Display" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_ENVIRONMENTAL_SENSOR
    { "environmental_sensor", "Environmental Sensor" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_EVENT_PUBLISHER
    { "event_publisher", "Event Publisher" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER
    { "http_server", "HTTP Server" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_JSON
    { "json", "JSON" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IMAGE
    { "image", "Image" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_THREAD
    { "thread", "Thread" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IMU
    { "imu", "IMU" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_IR
    { "ir", "IR" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_KNOB
    { "knob", "Knob" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LCD
    { "lcd", "LCD" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LCD_TOUCH && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT) && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUB_I2C_SUPPORT)
    { "lcd_touch", "LCD Touch" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LEDC
    { "ledc", "LEDC" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LED_STRIP
    { "led_strip", "LED Strip" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_LVGL
    { "lvgl", "LVGL" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_MAGNETOMETER
    { "magnetometer", "Magnetometer" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_SCI
    { "sci", "DFRobot SCI" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_STORAGE
    { "storage", "Storage" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_SYSTEM
    { "system", "System" },
#endif
#if CONFIG_APP_CLAW_LUA_MODULE_VISION && defined(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
    { "vision", "Vision" },
#endif
};

esp_err_t app_lua_modules_register_external(const app_lua_module_external_t *module)
{
    app_lua_module_external_t *slot = NULL;

    if (!module || !module->module_id || !module->module_id[0] || !module->reg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app_lua_find_entry(s_lua_module_entries,
                           sizeof(s_lua_module_entries) / sizeof(s_lua_module_entries[0]),
                           module->module_id) >= 0 ||
            app_lua_find_external_module(module->module_id) >= 0) {
        ESP_LOGW(TAG, "Lua module already registered: %s", module->module_id);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(app_lua_reserve_external_modules(s_external_module_count + 1),
                        TAG, "Failed to grow external Lua module registry");

    slot = &s_external_modules[s_external_module_count++];
    *slot = *module;
    if (!slot->display_name) {
        slot->display_name = slot->module_id;
    }
    return ESP_OK;
}

esp_err_t app_lua_modules_register(const app_claw_config_t *config, const char *fatfs_base_path)
{
    const size_t builtin_entry_count = sizeof(s_lua_module_entries) / sizeof(s_lua_module_entries[0]);
    const size_t entry_count = builtin_entry_count + s_external_module_count;
    app_lua_module_entry_t *entries = NULL;
    bool *selected_map = NULL;
    esp_err_t err = ESP_OK;
    size_t i;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    entries = calloc(entry_count > 0 ? entry_count : 1, sizeof(entries[0]));
    selected_map = calloc(entry_count > 0 ? entry_count : 1, sizeof(selected_map[0]));
    if (!entries || !selected_map) {
        free(entries);
        free(selected_map);
        return ESP_ERR_NO_MEM;
    }

    memcpy(entries, s_lua_module_entries, builtin_entry_count * sizeof(entries[0]));
    for (i = 0; i < s_external_module_count; i++) {
        entries[builtin_entry_count + i] = (app_lua_module_entry_t) {
            .module_id = s_external_modules[i].module_id,
            .display_name = s_external_modules[i].display_name,
            .reg = s_external_modules[i].reg,
        };
    }

    err = app_lua_build_module_map(config->enabled_lua_modules,
                                   entries,
                                   entry_count,
                                   selected_map);
    if (err != ESP_OK) {
        free(entries);
        free(selected_map);
        return err;
    }

    for (i = 0; i < entry_count; i++) {
        if (!selected_map[i]) {
            ESP_LOGI(TAG, "Skipping Lua module at init: %s", entries[i].module_id);
            continue;
        }

        err = entries[i].reg(fatfs_base_path);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register Lua module %s: %s",
                     entries[i].module_id,
                     esp_err_to_name(err));
            free(entries);
            free(selected_map);
            return err;
        }
    }

    free(entries);
    free(selected_map);
    return ESP_OK;
}

esp_err_t app_lua_modules_get_compiled_modules(const app_lua_module_info_t **modules,
                                               size_t *count)
{
    const size_t builtin_count = sizeof(s_lua_module_infos) / sizeof(s_lua_module_infos[0]);
    const size_t total_count = builtin_count + s_external_module_count;
    size_t i;

    if (!modules || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_external_module_count == 0) {
        *modules = s_lua_module_infos;
        *count = builtin_count;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(app_lua_reserve_module_infos(total_count > 0 ? total_count : 1),
                        TAG, "Failed to grow Lua module info cache");

    memcpy(s_module_infos, s_lua_module_infos, builtin_count * sizeof(s_module_infos[0]));
    for (i = 0; i < s_external_module_count; i++) {
        s_module_infos[builtin_count + i] = (app_lua_module_info_t) {
            .module_id = s_external_modules[i].module_id,
            .display_name = s_external_modules[i].display_name,
        };
    }

    *modules = s_module_infos;
    *count = total_count;
    return ESP_OK;
}
