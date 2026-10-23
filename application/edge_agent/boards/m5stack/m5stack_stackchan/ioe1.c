/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * StackChan "IOE1": PY32 based I2C IO expander on the CoreS3 internal I2C bus
 * (default 7-bit address 0x6F, ADD_SEL tied low). It provides 14 GPIOs, four
 * ADC channels, four PWM channels and an addressable-LED RAM block.
 *
 * This board device deliberately covers only what has to happen before any
 * script runs: wait for the PY32 to finish booting, then drive VM_EN so the
 * servo power rail is up. Everything else (ADC, PWM, the 12 Ring LEDs) is
 * reachable from Lua through the `lib_py32_ioe` library, which talks to the
 * same registers over the shared I2C bus.
 */

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "gen_board_device_custom.h"
#include "esp_board_manager_includes.h"
#include "ioe1.h"

static const char *TAG = "STACKCHAN_IOE1";

/* PY32 register map, mirrored from the M5Stack PY32IOExpander reference driver.
   16-bit fields are split into a low register (pins 0-7) and a high register
   (pins 8-13). */
#define IOE1_REG_VERSION    0x02
#define IOE1_REG_GPIO_M_L   0x03  /* mode: 0 = input, 1 = output */
#define IOE1_REG_GPIO_O_L   0x05  /* output level */
#define IOE1_REG_GPIO_I_L   0x07  /* input level */
#define IOE1_REG_GPIO_PU_L  0x09  /* pull-up enable */
#define IOE1_REG_GPIO_PD_L  0x0B  /* pull-down enable */

#define IOE1_REG_STRIDE     1     /* each field is a consecutive _L/_H pair */
#define IOE1_PINS_PER_REG   8
#define IOE1_MAX_PINS       14

#define IOE1_I2C_TIMEOUT_MS 1000

struct stackchan_ioe1_t {
    i2c_master_dev_handle_t i2c_dev;
};

static esp_err_t ioe1_read_reg(stackchan_ioe1_handle_t handle, uint8_t reg, uint8_t *out_value)
{
    return i2c_master_transmit_receive(handle->i2c_dev, &reg, 1, out_value, 1, IOE1_I2C_TIMEOUT_MS);
}

static esp_err_t ioe1_write_reg(stackchan_ioe1_handle_t handle, uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(handle->i2c_dev, buf, sizeof(buf), IOE1_I2C_TIMEOUT_MS);
}

/* Read-modify-write a single bit in the register pair that owns `pin`. */
static esp_err_t ioe1_write_pin_bit(stackchan_ioe1_handle_t handle, uint8_t reg_low, uint8_t pin, bool value)
{
    const uint8_t reg = (pin < IOE1_PINS_PER_REG) ? reg_low : (uint8_t)(reg_low + IOE1_REG_STRIDE);
    const uint8_t mask = (uint8_t)(1u << (pin % IOE1_PINS_PER_REG));

    uint8_t current = 0;
    esp_err_t err = ioe1_read_reg(handle, reg, &current);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t updated = value ? (uint8_t)(current | mask) : (uint8_t)(current & (uint8_t)~mask);
    if (updated == current) {
        return ESP_OK;
    }
    return ioe1_write_reg(handle, reg, updated);
}

static esp_err_t ioe1_read_pin_bit(stackchan_ioe1_handle_t handle, uint8_t reg_low, uint8_t pin, bool *out_value)
{
    const uint8_t reg = (pin < IOE1_PINS_PER_REG) ? reg_low : (uint8_t)(reg_low + IOE1_REG_STRIDE);
    const uint8_t mask = (uint8_t)(1u << (pin % IOE1_PINS_PER_REG));

    uint8_t current = 0;
    esp_err_t err = ioe1_read_reg(handle, reg, &current);
    if (err != ESP_OK) {
        return err;
    }
    *out_value = (current & mask) != 0;
    return ESP_OK;
}

/* The PY32 powers up slower than the ESP32-S3 and NAKs or returns a bogus
   version byte until its firmware is running. Poll until it answers. */
static esp_err_t ioe1_wait_ready(stackchan_ioe1_handle_t handle, int timeout_ms, int interval_ms)
{
    if (interval_ms <= 0) {
        interval_ms = 100;
    }

    int waited_ms = 0;
    while (true) {
        uint8_t version = 0;
        if (ioe1_read_reg(handle, IOE1_REG_VERSION, &version) == ESP_OK &&
            version != 0x00 && version != 0xFF) {
            ESP_LOGI(TAG, "PY32 IO expander ready, version 0x%02X (waited %d ms)", version, waited_ms);
            return ESP_OK;
        }

        if (waited_ms >= timeout_ms) {
            ESP_LOGE(TAG, "PY32 IO expander not responding after %d ms", waited_ms);
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(interval_ms));
        waited_ms += interval_ms;
    }
}

esp_err_t stackchan_ioe1_set_dir(stackchan_ioe1_handle_t handle, uint8_t pin, bool is_output)
{
    if (handle == NULL || pin >= IOE1_MAX_PINS) {
        return ESP_ERR_INVALID_ARG;
    }
    return ioe1_write_pin_bit(handle, IOE1_REG_GPIO_M_L, pin, is_output);
}

esp_err_t stackchan_ioe1_set_pull(stackchan_ioe1_handle_t handle, uint8_t pin, bool pull_up)
{
    if (handle == NULL || pin >= IOE1_MAX_PINS) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Clear the opposite direction first so the two never fight each other. */
    esp_err_t err = ioe1_write_pin_bit(handle, pull_up ? IOE1_REG_GPIO_PD_L : IOE1_REG_GPIO_PU_L, pin, false);
    if (err != ESP_OK) {
        return err;
    }
    return ioe1_write_pin_bit(handle, pull_up ? IOE1_REG_GPIO_PU_L : IOE1_REG_GPIO_PD_L, pin, true);
}

esp_err_t stackchan_ioe1_set_level(stackchan_ioe1_handle_t handle, uint8_t pin, bool level)
{
    if (handle == NULL || pin >= IOE1_MAX_PINS) {
        return ESP_ERR_INVALID_ARG;
    }
    return ioe1_write_pin_bit(handle, IOE1_REG_GPIO_O_L, pin, level);
}

esp_err_t stackchan_ioe1_get_level(stackchan_ioe1_handle_t handle, uint8_t pin, bool *out_level)
{
    if (handle == NULL || out_level == NULL || pin >= IOE1_MAX_PINS) {
        return ESP_ERR_INVALID_ARG;
    }
    return ioe1_read_pin_bit(handle, IOE1_REG_GPIO_I_L, pin, out_level);
}

esp_err_t stackchan_ioe1_read_version(stackchan_ioe1_handle_t handle, uint8_t *out_version)
{
    if (handle == NULL || out_version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ioe1_read_reg(handle, IOE1_REG_VERSION, out_version);
}

esp_err_t stackchan_ioe1_set_servo_power(stackchan_ioe1_handle_t handle, bool enabled)
{
    return stackchan_ioe1_set_level(handle, STACKCHAN_IOE1_PIN_VM_EN, enabled);
}

int stackchan_ioe1_init(void *config, int cfg_size, void **device_handle)
{
    if (config == NULL || device_handle == NULL) {
        ESP_LOGE(TAG, "Invalid arguments, config: %p, device_handle: %p", config, device_handle);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg_size != (int)sizeof(dev_custom_ioe1_config_t)) {
        ESP_LOGE(TAG, "Invalid config size: %d, expected %d",
                 cfg_size, (int)sizeof(dev_custom_ioe1_config_t));
        return ESP_ERR_INVALID_SIZE;
    }

    const dev_custom_ioe1_config_t *cfg = (const dev_custom_ioe1_config_t *)config;
    if (cfg->chip == NULL || strcmp(cfg->chip, "py32") != 0) {
        ESP_LOGE(TAG, "Unsupported IO expander chip: %s", cfg->chip ? cfg->chip : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_err_t err = esp_board_periph_get_handle(cfg->peripheral_name, (void **)&i2c_bus);
    if (err != ESP_OK || i2c_bus == NULL) {
        ESP_LOGE(TAG, "Failed to get I2C bus '%s' handle: %s",
                 cfg->peripheral_name ? cfg->peripheral_name : "(null)", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    stackchan_ioe1_handle_t handle = calloc(1, sizeof(struct stackchan_ioe1_t));
    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to allocate IOE1 handle");
        return ESP_ERR_NO_MEM;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint8_t)cfg->i2c_addr,
        .scl_speed_hz = (uint32_t)cfg->frequency,
    };
    err = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &handle->i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add PY32 at 0x%02X to the I2C bus: %s",
                 (unsigned)(uint8_t)cfg->i2c_addr, esp_err_to_name(err));
        free(handle);
        return err;
    }

    err = ioe1_wait_ready(handle, cfg->boot_timeout_ms, cfg->boot_retry_interval_ms);
    if (err != ESP_OK) {
        goto init_err;
    }

    const uint8_t vm_en_pin = (uint8_t)cfg->vm_en_pin;
    if (vm_en_pin >= IOE1_MAX_PINS) {
        ESP_LOGE(TAG, "vm_en_pin %u out of range (0-%d)", (unsigned)vm_en_pin, IOE1_MAX_PINS - 1);
        err = ESP_ERR_INVALID_ARG;
        goto init_err;
    }

    err = stackchan_ioe1_set_dir(handle, vm_en_pin, true);
    if (err == ESP_OK) {
        err = stackchan_ioe1_set_pull(handle, vm_en_pin, true);
    }
    if (err == ESP_OK) {
        err = stackchan_ioe1_set_level(handle, vm_en_pin, cfg->vm_en_on_init);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure VM_EN on pin %u: %s", (unsigned)vm_en_pin, esp_err_to_name(err));
        goto init_err;
    }

    ESP_LOGI(TAG, "Servo power rail (VM_EN, pin %u) %s",
             (unsigned)vm_en_pin, cfg->vm_en_on_init ? "enabled" : "left off");

    *device_handle = handle;
    return ESP_OK;

init_err:
    i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    return err;
}

int stackchan_ioe1_deinit(void *device_handle)
{
    if (device_handle == NULL) {
        ESP_LOGW(TAG, "IOE1 device handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    stackchan_ioe1_handle_t handle = (stackchan_ioe1_handle_t)device_handle;
    /* Drop the servo rail before releasing the device so the servos are not
       left powered by an orphaned expander state. */
    stackchan_ioe1_set_servo_power(handle, false);

    esp_err_t err = i2c_master_bus_rm_device(handle->i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to remove PY32 from the I2C bus: %s", esp_err_to_name(err));
    }
    free(handle);
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(ioe1, stackchan_ioe1_init, stackchan_ioe1_deinit);
