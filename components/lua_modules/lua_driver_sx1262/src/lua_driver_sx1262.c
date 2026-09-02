/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_sx1262.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "sx126x.h"

#define LUA_DRIVER_SX1262_METATABLE "sx1262.radio"
#define LUA_DRIVER_SX1262_MAX_PAYLOAD 255
#define LUA_DRIVER_SX1262_RX_CONTINUOUS_TIMEOUT 0xFFFFFFU
#define LUA_DRIVER_SX1262_DEFAULT_TIMEOUT_MS 5000
#define LUA_DRIVER_SX1262_MAX_TIMEOUT_MS 262143
#define LUA_DRIVER_SX1262_IRQ_POLL_MS 10
#define LUA_DRIVER_SX1262_HPD16A_TCXO_VOLTAGE 1.6
#define LUA_DRIVER_SX1262_TCXO_STARTUP_DELAY_US 5000.0

static const char *TAG = "lua_sx1262";

typedef struct {
    sx126x_handle_t radio;
    spi_host_device_t spi_host;
    gpio_num_t cs_gpio;
    gpio_num_t reset_gpio;
    gpio_num_t dio1_gpio;
    gpio_num_t busy_gpio;
    uint32_t frequency_hz;
    uint16_t preamble_length;
    uint8_t spreading_factor;
    uint16_t bandwidth_khz;
    uint8_t coding_rate;
    int8_t tx_power_dbm;
    uint8_t current_limit_ma;
    uint8_t sync_word;
    bool crc_enabled;
    bool invert_iq;
    bool reserved;
    bool open;
    bool receiving;
    uint8_t tx_buffer[LUA_DRIVER_SX1262_MAX_PAYLOAD];
    uint8_t rx_buffer[LUA_DRIVER_SX1262_MAX_PAYLOAD];
} lua_driver_sx1262_ud_t;

static portMUX_TYPE s_instance_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_instance_active;

static bool lua_driver_sx1262_reserve_instance(void)
{
    bool reserved = false;
    taskENTER_CRITICAL(&s_instance_lock);
    if (!s_instance_active) {
        s_instance_active = true;
        reserved = true;
    }
    taskEXIT_CRITICAL(&s_instance_lock);
    return reserved;
}

static void lua_driver_sx1262_release_instance(void)
{
    taskENTER_CRITICAL(&s_instance_lock);
    s_instance_active = false;
    taskEXIT_CRITICAL(&s_instance_lock);
}

static lua_Integer lua_driver_sx1262_get_integer(lua_State *L, int table_index,
                                                  const char *field, lua_Integer default_value,
                                                  lua_Integer min_value, lua_Integer max_value,
                                                  bool required)
{
    lua_Integer value = default_value;
    lua_getfield(L, table_index, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (required) {
            luaL_error(L, "sx1262 config '%s' is required", field);
        }
        return value;
    }
    value = luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (value < min_value || value > max_value) {
        luaL_error(L, "sx1262 config '%s' must be in range %lld-%lld", field,
                   (long long)min_value, (long long)max_value);
    }
    return value;
}

static lua_Number lua_driver_sx1262_get_number(lua_State *L, int table_index,
                                                const char *field, lua_Number default_value,
                                                lua_Number min_value, lua_Number max_value)
{
    lua_Number value = default_value;
    lua_getfield(L, table_index, field);
    if (!lua_isnil(L, -1)) {
        value = luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);
    if (value < min_value || value > max_value) {
        luaL_error(L, "sx1262 config '%s' is out of range", field);
    }
    return value;
}

static bool lua_driver_sx1262_get_bool(lua_State *L, int table_index,
                                       const char *field, bool default_value)
{
    bool value = default_value;
    lua_getfield(L, table_index, field);
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TBOOLEAN);
        value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static sx126x_lora_bandwidth_t lua_driver_sx1262_parse_bandwidth(lua_State *L,
                                                                 lua_Integer bandwidth_khz)
{
    switch (bandwidth_khz) {
    case 7: return SX126X_LORA_BANDWIDTH_7;
    case 10: return SX126X_LORA_BANDWIDTH_10;
    case 15: return SX126X_LORA_BANDWIDTH_15;
    case 20: return SX126X_LORA_BANDWIDTH_20;
    case 31: return SX126X_LORA_BANDWIDTH_31;
    case 41: return SX126X_LORA_BANDWIDTH_41;
    case 62: return SX126X_LORA_BANDWIDTH_62;
    case 125: return SX126X_LORA_BANDWIDTH_125;
    case 250: return SX126X_LORA_BANDWIDTH_250;
    case 500: return SX126X_LORA_BANDWIDTH_500;
    default:
        luaL_error(L, "sx1262 bandwidth_khz must be 7, 10, 15, 20, 31, 41, 62, 125, 250, or 500");
        return SX126X_LORA_BANDWIDTH_125;
    }
}

static sx126x_lora_coding_rate_t lua_driver_sx1262_parse_coding_rate(lua_State *L,
                                                                     lua_Integer denominator)
{
    switch (denominator) {
    case 5: return SX126X_LORA_CODING_RATE_4_5;
    case 6: return SX126X_LORA_CODING_RATE_4_6;
    case 7: return SX126X_LORA_CODING_RATE_4_7;
    case 8: return SX126X_LORA_CODING_RATE_4_8;
    default:
        luaL_error(L, "sx1262 coding_rate must be 5, 6, 7, or 8 (for 4/5 through 4/8)");
        return SX126X_LORA_CODING_RATE_4_6;
    }
}

static void lua_driver_sx1262_destroy_resources(lua_driver_sx1262_ud_t *ud)
{
    if (!ud) {
        return;
    }

    if (ud->open) {
        (void)sx126x_set_op_mode_standby(&ud->radio, false);
    }
    if (ud->radio.dio1 != GPIO_NUM_NC) {
        (void)gpio_isr_handler_remove(ud->radio.dio1);
    }
    if (ud->radio.busy != GPIO_NUM_NC) {
        (void)gpio_isr_handler_remove(ud->radio.busy);
    }
    if (ud->radio.device) {
        (void)spi_bus_remove_device(ud->radio.device);
    }
    if (ud->radio.interrupt_semaphore) {
        vSemaphoreDelete(ud->radio.interrupt_semaphore);
    }
    if (ud->radio.busy_semaphore) {
        vSemaphoreDelete(ud->radio.busy_semaphore);
    }
    if (ud->radio.spi_semaphore) {
        vSemaphoreDelete(ud->radio.spi_semaphore);
    }
    if (ud->radio.reset != GPIO_NUM_NC) {
        (void)gpio_reset_pin(ud->radio.reset);
    }
    if (ud->radio.dio1 != GPIO_NUM_NC) {
        (void)gpio_reset_pin(ud->radio.dio1);
    }
    if (ud->radio.busy != GPIO_NUM_NC) {
        (void)gpio_reset_pin(ud->radio.busy);
    }

    memset(&ud->radio, 0, sizeof(ud->radio));
    ud->radio.reset = GPIO_NUM_NC;
    ud->radio.dio1 = GPIO_NUM_NC;
    ud->radio.busy = GPIO_NUM_NC;
    ud->open = false;
    ud->receiving = false;
    if (ud->reserved) {
        lua_driver_sx1262_release_instance();
        ud->reserved = false;
    }
}

static lua_driver_sx1262_ud_t *lua_driver_sx1262_get_ud(lua_State *L, int index)
{
    lua_driver_sx1262_ud_t *ud = (lua_driver_sx1262_ud_t *)luaL_checkudata(
        L, index, LUA_DRIVER_SX1262_METATABLE);
    if (!ud || !ud->open) {
        luaL_error(L, "sx1262: invalid or closed radio");
    }
    return ud;
}

static int lua_driver_sx1262_fail_new(lua_State *L, lua_driver_sx1262_ud_t *ud,
                                      const char *operation, esp_err_t err)
{
    lua_driver_sx1262_destroy_resources(ud);
    return luaL_error(L, "sx1262 %s failed: %s", operation, esp_err_to_name(err));
}

static int lua_driver_sx1262_fail_radio(lua_State *L, lua_driver_sx1262_ud_t *ud,
                                        const char *operation, uint16_t device_errors)
{
    ESP_LOGE(TAG, "%s failed, device errors=0x%04X", operation, device_errors);
    lua_driver_sx1262_destroy_resources(ud);
    return luaL_error(L, "sx1262 %s failed: device_errors=0x%04X",
                      operation, device_errors);
}

static esp_err_t lua_driver_sx1262_read_device_errors(lua_driver_sx1262_ud_t *ud,
                                                       uint16_t *device_errors)
{
    esp_err_t err = sx126x_set_op_mode_standby(&ud->radio, false);
    if (err == ESP_OK) {
        err = sx126x_get_device_errors(&ud->radio, device_errors);
    }
    return err;
}

static bool lua_driver_sx1262_status_is_valid(uint8_t command_status, uint8_t chip_mode)
{
    bool command_ok = command_status == SX126X_COMMAND_STATUS_DATA_AVAILABLE ||
                      command_status == SX126X_COMMAND_STATUS_TX_DONE;
    return command_ok &&
           chip_mode >= SX126X_CHIP_MODE_STDBY_RC &&
           chip_mode <= SX126X_CHIP_MODE_TX;
}

static int lua_driver_sx1262_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_Integer spi_host = lua_driver_sx1262_get_integer(L, 1, "spi_host", SPI3_HOST,
                                                         SPI2_HOST, SPI3_HOST, false);
    lua_Integer cs_gpio = lua_driver_sx1262_get_integer(L, 1, "cs_gpio", -1, 0,
                                                        GPIO_NUM_MAX - 1, true);
    lua_Integer reset_gpio = lua_driver_sx1262_get_integer(L, 1, "reset_gpio", -1, 0,
                                                           GPIO_NUM_MAX - 1, true);
    lua_Integer dio1_gpio = lua_driver_sx1262_get_integer(L, 1, "dio1_gpio", -1, 0,
                                                          GPIO_NUM_MAX - 1, true);
    lua_Integer busy_gpio = lua_driver_sx1262_get_integer(L, 1, "busy_gpio", -1, 0,
                                                          GPIO_NUM_MAX - 1, true);
    lua_Integer frequency_hz = lua_driver_sx1262_get_integer(L, 1, "frequency_hz", 0,
                                                             150000000, 960000000, true);
    lua_Integer bandwidth_khz = lua_driver_sx1262_get_integer(L, 1, "bandwidth_khz", 125,
                                                              7, 500, false);
    lua_Integer spreading_factor = lua_driver_sx1262_get_integer(L, 1, "spreading_factor", 9,
                                                                 5, 12, false);
    lua_Integer coding_rate = lua_driver_sx1262_get_integer(L, 1, "coding_rate", 6,
                                                            5, 8, false);
    lua_Integer tx_power_dbm = lua_driver_sx1262_get_integer(L, 1, "tx_power_dbm", 14,
                                                             -9, 22, false);
    lua_Integer current_limit_ma = lua_driver_sx1262_get_integer(
        L, 1, "current_limit_ma", 140, 60, 140, false);
    lua_Integer preamble_length = lua_driver_sx1262_get_integer(L, 1, "preamble_length", 16,
                                                                1, UINT16_MAX, false);
    lua_Integer sync_word = lua_driver_sx1262_get_integer(L, 1, "sync_word", 0xAB,
                                                          0, UINT8_MAX, false);
    lua_Number tcxo_voltage = lua_driver_sx1262_get_number(
        L, 1, "tcxo_voltage", LUA_DRIVER_SX1262_HPD16A_TCXO_VOLTAGE, 0.0, 3.3);
    lua_Number tcxo_delay_us = lua_driver_sx1262_get_number(
        L, 1, "tcxo_delay_us", LUA_DRIVER_SX1262_TCXO_STARTUP_DELAY_US,
                                                            0.0, 262143984.0);
    bool use_dcdc = lua_driver_sx1262_get_bool(L, 1, "use_dcdc", true);
    bool dio2_rf_switch = lua_driver_sx1262_get_bool(L, 1, "dio2_rf_switch", true);
    bool crc_enabled = lua_driver_sx1262_get_bool(L, 1, "crc", true);
    bool invert_iq = lua_driver_sx1262_get_bool(L, 1, "invert_iq", false);

    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)cs_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)reset_gpio)) {
        return luaL_error(L, "sx1262 cs_gpio and reset_gpio must be output-capable GPIOs");
    }
    if (!GPIO_IS_VALID_GPIO((gpio_num_t)dio1_gpio) ||
        !GPIO_IS_VALID_GPIO((gpio_num_t)busy_gpio)) {
        return luaL_error(L, "sx1262 dio1_gpio and busy_gpio must be valid GPIOs");
    }
    if (cs_gpio == reset_gpio || cs_gpio == dio1_gpio || cs_gpio == busy_gpio ||
        reset_gpio == dio1_gpio || reset_gpio == busy_gpio || dio1_gpio == busy_gpio) {
        return luaL_error(L, "sx1262 control GPIOs must be unique");
    }

    sx126x_lora_bandwidth_t bandwidth = lua_driver_sx1262_parse_bandwidth(L, bandwidth_khz);
    sx126x_lora_coding_rate_t cr = lua_driver_sx1262_parse_coding_rate(L, coding_rate);

    lua_driver_sx1262_ud_t *ud = (lua_driver_sx1262_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->radio.reset = GPIO_NUM_NC;
    ud->radio.dio1 = GPIO_NUM_NC;
    ud->radio.busy = GPIO_NUM_NC;
    ud->spi_host = (spi_host_device_t)spi_host;
    ud->cs_gpio = (gpio_num_t)cs_gpio;
    ud->reset_gpio = (gpio_num_t)reset_gpio;
    ud->dio1_gpio = (gpio_num_t)dio1_gpio;
    ud->busy_gpio = (gpio_num_t)busy_gpio;
    ud->frequency_hz = (uint32_t)frequency_hz;
    ud->preamble_length = (uint16_t)preamble_length;
    ud->spreading_factor = (uint8_t)spreading_factor;
    ud->bandwidth_khz = (uint16_t)bandwidth_khz;
    ud->coding_rate = (uint8_t)coding_rate;
    ud->tx_power_dbm = (int8_t)tx_power_dbm;
    ud->current_limit_ma = (uint8_t)current_limit_ma;
    ud->sync_word = (uint8_t)sync_word;
    ud->crc_enabled = crc_enabled;
    ud->invert_iq = invert_iq;
    luaL_getmetatable(L, LUA_DRIVER_SX1262_METATABLE);
    lua_setmetatable(L, -2);

    if (!lua_driver_sx1262_reserve_instance()) {
        return luaL_error(L, "sx1262 supports one active radio instance");
    }
    ud->reserved = true;

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return lua_driver_sx1262_fail_new(L, ud, "gpio_install_isr_service", err);
    }

    err = sx126x_init(&ud->radio, ud->spi_host, ud->cs_gpio, ud->reset_gpio,
                      ud->dio1_gpio, ud->busy_gpio);
    if (err != ESP_OK) {
        /* sx126x_init() deletes busy_semaphore itself if the following
         * interrupt_semaphore allocation fails, but does not clear the field. */
        if (!ud->radio.interrupt_semaphore && ud->radio.busy_semaphore) {
            ud->radio.busy_semaphore = NULL;
        }
        return lua_driver_sx1262_fail_new(L, ud, "init", err);
    }
    ud->open = true;

#define SX1262_INIT_STEP(name, expression) \
    do { \
        err = (expression); \
        if (err != ESP_OK) { \
            return lua_driver_sx1262_fail_new(L, ud, (name), err); \
        } \
    } while (0)

    /* Match the initialization sequence used by the board's RadioLib 7.1.2
     * example. HPD16A uses a 1.6 V DIO3-controlled TCXO. */
    char version[SX126X_VERSION_STRING_LENGTH + 1] = {0};
    bool chip_found = false;
    uint8_t detect_command_status = 0;
    uint8_t detect_chip_mode = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        /* RadioLib findChip() begins every attempt with a hard reset, then its
         * verified reset path enters standby before reading the version ROM. */
        SX1262_INIT_STEP("chip reset", sx1262_reset(&ud->radio));
        SX1262_INIT_STEP("chip standby", sx126x_set_op_mode_standby(&ud->radio, false));
        memset(version, 0, sizeof(version));
        err = sx126x_read_version_string(&ud->radio, version, SX126X_VERSION_STRING_LENGTH);
        if (err == ESP_OK && memcmp(version, "SX1261", 6) == 0) {
            chip_found = true;
            break;
        }

        /* Some ESP-IDF SPI hosts do not return the version-ROM burst exactly
         * like Arduino SPItransfer(). GetStatus still provides a reliable
         * electrical/protocol check and rejects floating MISO (0x00/0xFF). */
        detect_command_status = 0;
        detect_chip_mode = 0;
        esp_err_t status_err = sx126x_get_status(
            &ud->radio, &detect_command_status, &detect_chip_mode);
        if (status_err == ESP_OK && lua_driver_sx1262_status_is_valid(
                detect_command_status, detect_chip_mode)) {
            ESP_LOGW(TAG,
                     "version ROM mismatch; accepting SX1262 status command=%u mode=%u",
                     (unsigned int)detect_command_status, (unsigned int)detect_chip_mode);
            chip_found = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!chip_found) {
        ESP_LOGE(TAG,
                 "chip detect failed, version prefix=%02X %02X %02X %02X %02X %02X status=%u mode=%u",
                 (unsigned int)(uint8_t)version[0], (unsigned int)(uint8_t)version[1],
                 (unsigned int)(uint8_t)version[2], (unsigned int)(uint8_t)version[3],
                 (unsigned int)(uint8_t)version[4], (unsigned int)(uint8_t)version[5],
                 (unsigned int)detect_command_status, (unsigned int)detect_chip_mode);
        return lua_driver_sx1262_fail_new(L, ud, "chip detect", ESP_ERR_NOT_FOUND);
    }

    SX1262_INIT_STEP("clear device errors", sx126x_clear_device_errors(&ud->radio, NULL, NULL));
    if (tcxo_voltage > 0.0) {
        SX1262_INIT_STEP("TCXO", sx126x_set_dio3_as_txco_ctrl(
            &ud->radio, (float)tcxo_voltage, (float)tcxo_delay_us));
    }
    SX1262_INIT_STEP("buffer base", sx126x_set_buffer_base_address(&ud->radio, 0, 0));
    SX1262_INIT_STEP("packet type", sx126x_set_packet_type(&ud->radio, SX126X_PACKET_TYPE_LORA));
    SX1262_INIT_STEP("fallback", sx126x_set_rx_tx_fallback_mode(
        &ud->radio, SX126X_FALLBACK_MODE_STDBY_RC));
    SX1262_INIT_STEP("clear IRQ", sx126x_clear_irq_status(&ud->radio, SX126X_IRQ_ALL));
    SX1262_INIT_STEP("disable IRQ", sx126x_set_dio_irq_params(&ud->radio, 0, 0, 0, 0));
    SX1262_INIT_STEP("calibration", sx126x_calibrate(
        &ud->radio, true, true, true, true, true, true, true));
    vTaskDelay(pdMS_TO_TICKS(5));

    uint16_t device_errors = 0;
    SX1262_INIT_STEP("calibration status", lua_driver_sx1262_read_device_errors(
        ud, &device_errors));
    if (device_errors != 0) {
        return lua_driver_sx1262_fail_radio(L, ud, "calibration", device_errors);
    }

    SX1262_INIT_STEP("regulator", sx126x_set_regulator_mode(&ud->radio, use_dcdc));
    SX1262_INIT_STEP("RF switch", sx126x_set_dio2_as_rf_switch_ctrl(&ud->radio, dio2_rf_switch));
    uint8_t clamp_config = 0;
    SX1262_INIT_STEP("PA clamp read", sx126x_read_register(
        &ud->radio, SX126X_REG_TX_CLAMP_CONFIG, &clamp_config, 1));
    clamp_config |= 0x1E;
    SX1262_INIT_STEP("PA clamp write", sx126x_write_register(
        &ud->radio, SX126X_REG_TX_CLAMP_CONFIG, &clamp_config, 1));
    SX1262_INIT_STEP("PA config", sx126x_set_pa_config(&ud->radio, 0x04, 0x07, false));
    uint8_t ocp_value = (uint8_t)(((unsigned int)ud->current_limit_ma * 2U + 2U) / 5U);
    SX1262_INIT_STEP("current limit", sx126x_write_register(
        &ud->radio, SX126X_REG_OCP_CONFIGURATION, &ocp_value, 1));
    SX1262_INIT_STEP("TX params", sx126x_set_tx_params(&ud->radio, ud->tx_power_dbm, true, 200));
    SX1262_INIT_STEP("modulation", sx126x_set_modulation_params_lora(
        &ud->radio, (sx126x_lora_spreading_factor_t)ud->spreading_factor,
        bandwidth, cr, true, false));
    SX1262_INIT_STEP("packet params", sx126x_set_packet_params_lora_variable_length(
        &ud->radio, ud->preamble_length, ud->crc_enabled, ud->invert_iq));
    SX1262_INIT_STEP("sync word", sx126x_set_sync_word(&ud->radio, ud->sync_word));
    SX1262_INIT_STEP("frequency", sx126x_set_rf_frequency(&ud->radio, ud->frequency_hz));

    SX1262_INIT_STEP("clear IRQ", sx126x_clear_irq_status(&ud->radio, SX126X_IRQ_ALL));

    sx126x_packet_type_t packet_type = SX126X_PACKET_TYPE_GFSK;
    SX1262_INIT_STEP("packet type verify", sx126x_get_packet_type(&ud->radio, &packet_type));
    if (packet_type != SX126X_PACKET_TYPE_LORA) {
        return lua_driver_sx1262_fail_new(L, ud, "packet type verify", ESP_ERR_INVALID_RESPONSE);
    }
    SX1262_INIT_STEP("final status", lua_driver_sx1262_read_device_errors(
        ud, &device_errors));
    if (device_errors != 0) {
        return lua_driver_sx1262_fail_radio(L, ud, "configuration", device_errors);
    }
#undef SX1262_INIT_STEP

    return 1;
}

static size_t lua_driver_sx1262_read_payload(lua_State *L, int index,
                                             uint8_t buffer[LUA_DRIVER_SX1262_MAX_PAYLOAD])
{
    if (lua_type(L, index) == LUA_TSTRING) {
        size_t length = 0;
        const char *data = lua_tolstring(L, index, &length);
        if (length > LUA_DRIVER_SX1262_MAX_PAYLOAD) {
            luaL_error(L, "sx1262 payload must be at most %d bytes", LUA_DRIVER_SX1262_MAX_PAYLOAD);
        }
        if (length > 0) {
            memcpy(buffer, data, length);
        }
        return length;
    }
    if (lua_type(L, index) == LUA_TTABLE) {
        lua_Integer length = luaL_len(L, index);
        if (length < 0 || length > LUA_DRIVER_SX1262_MAX_PAYLOAD) {
            luaL_error(L, "sx1262 payload table must contain 0-%d bytes", LUA_DRIVER_SX1262_MAX_PAYLOAD);
        }
        for (lua_Integer i = 0; i < length; ++i) {
            lua_rawgeti(L, index, i + 1);
            lua_Integer value = luaL_checkinteger(L, -1);
            lua_pop(L, 1);
            if (value < 0 || value > UINT8_MAX) {
                luaL_error(L, "sx1262 payload byte #%lld must be in range 0-255", (long long)(i + 1));
            }
            buffer[i] = (uint8_t)value;
        }
        return (size_t)length;
    }
    luaL_error(L, "sx1262 payload must be a string or table of bytes");
    return 0;
}

static TickType_t lua_driver_sx1262_timeout_ticks(lua_State *L, int index, lua_Integer default_ms,
                                                   lua_Integer *out_ms)
{
    lua_Integer timeout_ms = luaL_optinteger(L, index, default_ms);
    if (timeout_ms < 0 || timeout_ms > LUA_DRIVER_SX1262_MAX_TIMEOUT_MS) {
        luaL_error(L, "sx1262 timeout_ms must be in range 0-%d", LUA_DRIVER_SX1262_MAX_TIMEOUT_MS);
    }
    if (out_ms) {
        *out_ms = timeout_ms;
    }
    return timeout_ms == 0 ? 0 : pdMS_TO_TICKS((TickType_t)timeout_ms);
}

static void lua_driver_sx1262_drain_irq(lua_driver_sx1262_ud_t *ud)
{
    while (xSemaphoreTake(ud->radio.interrupt_semaphore, 0) == pdTRUE) {
    }
}

static esp_err_t lua_driver_sx1262_wait_irq(lua_driver_sx1262_ud_t *ud,
                                            uint16_t expected_mask,
                                            TickType_t timeout_ticks,
                                            uint16_t *out_irq_status)
{
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t poll_ticks = pdMS_TO_TICKS(LUA_DRIVER_SX1262_IRQ_POLL_MS);

    if (poll_ticks == 0) {
        poll_ticks = 1;
    }
    for (;;) {
        uint16_t irq_status = 0;
        esp_err_t err = sx126x_get_irq_status(&ud->radio, &irq_status, NULL, NULL);
        if (err != ESP_OK) {
            return err;
        }
        if (irq_status & expected_mask) {
            if (out_irq_status) {
                *out_irq_status = irq_status;
            }
            return ESP_OK;
        }
        if (timeout_ticks == 0) {
            return ESP_ERR_TIMEOUT;
        }

        TickType_t elapsed_ticks = xTaskGetTickCount() - start_ticks;
        if (elapsed_ticks >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        TickType_t remaining_ticks = timeout_ticks - elapsed_ticks;
        TickType_t wait_ticks = remaining_ticks < poll_ticks ? remaining_ticks : poll_ticks;
        err = sx126x_irq_wait(&ud->radio, wait_ticks);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            return err;
        }
    }
}

static int lua_driver_sx1262_push_operation_error(lua_State *L, const char *operation, esp_err_t err)
{
    lua_pushnil(L);
    lua_pushfstring(L, "%s: %s", operation, esp_err_to_name(err));
    return 2;
}

static int lua_driver_sx1262_send(lua_State *L)
{
    lua_driver_sx1262_ud_t *ud = lua_driver_sx1262_get_ud(L, 1);
    size_t length = lua_driver_sx1262_read_payload(L, 2, ud->tx_buffer);
    lua_Integer timeout_ms = 0;
    TickType_t timeout_ticks = lua_driver_sx1262_timeout_ticks(
        L, 3, LUA_DRIVER_SX1262_DEFAULT_TIMEOUT_MS, &timeout_ms);
    uint32_t radio_timeout = timeout_ms == 0 ? 1U : (uint32_t)timeout_ms * 64U;
    uint16_t irq_status = 0;

    esp_err_t err = sx126x_set_op_mode_standby(&ud->radio, false);
    if (err == ESP_OK) err = sx126x_clear_irq_status(&ud->radio, SX126X_IRQ_ALL);
    if (err == ESP_OK) {
        lua_driver_sx1262_drain_irq(ud);
        err = sx126x_set_packet_params_lora(&ud->radio, ud->preamble_length, false,
                                             (uint8_t)length, ud->crc_enabled, ud->invert_iq);
    }
    if (err == ESP_OK) err = sx126x_write_buffer(&ud->radio, 0, ud->tx_buffer, length);
    if (err == ESP_OK) {
        err = sx126x_set_dio_irq_params(&ud->radio,
                                        SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT,
                                        SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT, 0, 0);
    }
    if (err == ESP_OK) err = sx126x_set_op_mode_tx(&ud->radio, radio_timeout);
    ud->receiving = false;
    if (err != ESP_OK) {
        return lua_driver_sx1262_push_operation_error(L, "send", err);
    }

    err = lua_driver_sx1262_wait_irq(ud, SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT,
                                     timeout_ticks, &irq_status);
    if (err == ESP_ERR_TIMEOUT) {
        (void)sx126x_set_op_mode_standby(&ud->radio, false);
        lua_pushnil(L);
        lua_pushstring(L, "timeout");
        return 2;
    }
    if (err != ESP_OK) {
        return lua_driver_sx1262_push_operation_error(L, "send wait", err);
    }
    err = sx126x_clear_irq_status(&ud->radio, irq_status);
    if (err != ESP_OK) {
        return lua_driver_sx1262_push_operation_error(L, "send IRQ", err);
    }
    if (irq_status & SX126X_IRQ_TIMEOUT) {
        lua_pushnil(L);
        lua_pushstring(L, "radio_timeout");
        return 2;
    }
    if (!(irq_status & SX126X_IRQ_TX_DONE)) {
        lua_pushnil(L);
        lua_pushfstring(L, "unexpected_irq_%d", (int)irq_status);
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static esp_err_t lua_driver_sx1262_start_receive_internal(lua_driver_sx1262_ud_t *ud)
{
    esp_err_t err = sx126x_set_op_mode_standby(&ud->radio, false);
    if (err == ESP_OK) {
        err = sx126x_set_packet_params_lora_variable_length(
            &ud->radio, ud->preamble_length, ud->crc_enabled, ud->invert_iq);
    }
    if (err == ESP_OK) err = sx126x_clear_irq_status(&ud->radio, SX126X_IRQ_ALL);
    if (err == ESP_OK) {
        lua_driver_sx1262_drain_irq(ud);
        uint16_t irq_mask = SX126X_IRQ_RX_DONE | SX126X_IRQ_HEADER_ERROR |
                            SX126X_IRQ_CRC_ERROR | SX126X_IRQ_TIMEOUT;
        err = sx126x_set_dio_irq_params(&ud->radio, irq_mask, irq_mask, 0, 0);
    }
    if (err == ESP_OK) {
        err = sx126x_set_op_mode_rx(&ud->radio, LUA_DRIVER_SX1262_RX_CONTINUOUS_TIMEOUT);
    }
    ud->receiving = err == ESP_OK;
    return err;
}

static int lua_driver_sx1262_start_receive(lua_State *L)
{
    lua_driver_sx1262_ud_t *ud = lua_driver_sx1262_get_ud(L, 1);
    esp_err_t err = lua_driver_sx1262_start_receive_internal(ud);
    if (err != ESP_OK) {
        return lua_driver_sx1262_push_operation_error(L, "start_receive", err);
    }
    lua_pushboolean(L, true);
    return 1;
}

static int lua_driver_sx1262_receive(lua_State *L)
{
    lua_driver_sx1262_ud_t *ud = lua_driver_sx1262_get_ud(L, 1);
    TickType_t timeout_ticks = lua_driver_sx1262_timeout_ticks(L, 2, 0, NULL);
    if (!ud->receiving) {
        return luaL_error(L, "sx1262 receive requires start_receive() first");
    }

    uint16_t expected_mask = SX126X_IRQ_RX_DONE | SX126X_IRQ_HEADER_ERROR |
                             SX126X_IRQ_CRC_ERROR | SX126X_IRQ_TIMEOUT;
    uint16_t irq_status = 0;
    esp_err_t err = lua_driver_sx1262_wait_irq(ud, expected_mask, timeout_ticks, &irq_status);
    if (err == ESP_ERR_TIMEOUT) {
        lua_pushnil(L);
        lua_pushstring(L, "timeout");
        return 2;
    }
    if (err != ESP_OK) {
        return lua_driver_sx1262_push_operation_error(L, "receive wait", err);
    }

    err = sx126x_clear_irq_status(&ud->radio, irq_status);
    if (err != ESP_OK) {
        return lua_driver_sx1262_push_operation_error(L, "receive IRQ", err);
    }
    if (irq_status & SX126X_IRQ_CRC_ERROR) {
        lua_pushnil(L);
        lua_pushstring(L, "crc_error");
        return 2;
    }
    if (irq_status & SX126X_IRQ_HEADER_ERROR) {
        lua_pushnil(L);
        lua_pushstring(L, "header_error");
        return 2;
    }
    if (irq_status & SX126X_IRQ_TIMEOUT) {
        ud->receiving = false;
        lua_pushnil(L);
        lua_pushstring(L, "radio_timeout");
        return 2;
    }
    if (!(irq_status & SX126X_IRQ_RX_DONE)) {
        lua_pushnil(L);
        lua_pushfstring(L, "unexpected_irq_%d", (int)irq_status);
        return 2;
    }

    uint8_t payload_length = 0;
    uint8_t start_pointer = 0;
    float snr = 0.0f;
    float rssi = 0.0f;
    float signal_rssi = 0.0f;
    err = sx126x_get_rx_buffer_status(&ud->radio, &payload_length, &start_pointer);
    if (err == ESP_OK) {
        err = sx126x_read_buffer(&ud->radio, start_pointer, ud->rx_buffer, payload_length);
    }
    if (err == ESP_OK) {
        err = sx126x_get_packet_status_lora(&ud->radio, &snr, &rssi, &signal_rssi);
    }
    if (err != ESP_OK) {
        return lua_driver_sx1262_push_operation_error(L, "receive data", err);
    }
    if (snr >= 32.0f) {
        snr -= 64.0f;
    }

    lua_newtable(L);
    lua_pushlstring(L, (const char *)ud->rx_buffer, payload_length);
    lua_setfield(L, -2, "data");
    lua_pushinteger(L, payload_length);
    lua_setfield(L, -2, "length");
    lua_pushnumber(L, rssi);
    lua_setfield(L, -2, "rssi");
    lua_pushnumber(L, snr);
    lua_setfield(L, -2, "snr");
    lua_pushnumber(L, signal_rssi);
    lua_setfield(L, -2, "signal_rssi");
    lua_newtable(L);
    for (uint8_t i = 0; i < payload_length; ++i) {
        lua_pushinteger(L, ud->rx_buffer[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "bytes");
    return 1;
}

static const char *lua_driver_sx1262_chip_mode_name(uint8_t mode)
{
    switch (mode) {
    case SX126X_CHIP_MODE_STDBY_RC: return "standby_rc";
    case SX126X_CHIP_MODE_STDBY_XOSC: return "standby_xosc";
    case SX126X_CHIP_MODE_FS: return "frequency_synthesis";
    case SX126X_CHIP_MODE_RX: return "receive";
    case SX126X_CHIP_MODE_TX: return "transmit";
    default: return "unknown";
    }
}

static int lua_driver_sx1262_status(lua_State *L)
{
    lua_driver_sx1262_ud_t *ud = lua_driver_sx1262_get_ud(L, 1);
    uint8_t command_status = 0;
    uint8_t chip_mode = 0;
    uint16_t device_errors = 0;
    esp_err_t err = sx126x_get_status(&ud->radio, &command_status, &chip_mode);
    if (err == ESP_OK) err = sx126x_get_device_errors(&ud->radio, &device_errors);
    if (err != ESP_OK) {
        return luaL_error(L, "sx1262 status failed: %s", esp_err_to_name(err));
    }

    lua_newtable(L);
    lua_pushstring(L, lua_driver_sx1262_chip_mode_name(chip_mode));
    lua_setfield(L, -2, "mode");
    lua_pushinteger(L, command_status);
    lua_setfield(L, -2, "command_status");
    lua_pushinteger(L, device_errors);
    lua_setfield(L, -2, "device_errors");
    lua_pushboolean(L, ud->receiving);
    lua_setfield(L, -2, "receiving");
    lua_pushinteger(L, ud->frequency_hz);
    lua_setfield(L, -2, "frequency_hz");
    lua_pushinteger(L, ud->bandwidth_khz);
    lua_setfield(L, -2, "bandwidth_khz");
    lua_pushinteger(L, ud->spreading_factor);
    lua_setfield(L, -2, "spreading_factor");
    lua_pushinteger(L, ud->coding_rate);
    lua_setfield(L, -2, "coding_rate");
    lua_pushinteger(L, ud->tx_power_dbm);
    lua_setfield(L, -2, "tx_power_dbm");
    lua_pushinteger(L, ud->current_limit_ma);
    lua_setfield(L, -2, "current_limit_ma");
    lua_pushinteger(L, ud->spi_host);
    lua_setfield(L, -2, "spi_host");
    return 1;
}

static int lua_driver_sx1262_standby(lua_State *L)
{
    lua_driver_sx1262_ud_t *ud = lua_driver_sx1262_get_ud(L, 1);
    esp_err_t err = sx126x_set_op_mode_standby(&ud->radio, false);
    if (err != ESP_OK) {
        return lua_driver_sx1262_push_operation_error(L, "standby", err);
    }
    ud->receiving = false;
    lua_pushboolean(L, true);
    return 1;
}

static int lua_driver_sx1262_close(lua_State *L)
{
    lua_driver_sx1262_ud_t *ud = (lua_driver_sx1262_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_SX1262_METATABLE);
    lua_driver_sx1262_destroy_resources(ud);
    return 0;
}

static int lua_driver_sx1262_gc(lua_State *L)
{
    lua_driver_sx1262_ud_t *ud = (lua_driver_sx1262_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_SX1262_METATABLE);
    lua_driver_sx1262_destroy_resources(ud);
    return 0;
}

int luaopen_sx1262(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_SX1262_METATABLE)) {
        lua_pushcfunction(L, lua_driver_sx1262_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_sx1262_send);
        lua_setfield(L, -2, "send");
        lua_pushcfunction(L, lua_driver_sx1262_start_receive);
        lua_setfield(L, -2, "start_receive");
        lua_pushcfunction(L, lua_driver_sx1262_receive);
        lua_setfield(L, -2, "receive");
        lua_pushcfunction(L, lua_driver_sx1262_status);
        lua_setfield(L, -2, "status");
        lua_pushcfunction(L, lua_driver_sx1262_standby);
        lua_setfield(L, -2, "standby");
        lua_pushcfunction(L, lua_driver_sx1262_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_sx1262_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_driver_sx1262_register(void)
{
    return cap_lua_register_module("sx1262", luaopen_sx1262);
}
