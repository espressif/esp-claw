/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw_hw_bridge.h"

#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_board_manager.h"
#include "esp_log.h"
#include "hal/spi_types.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#if SOC_SDMMC_HOST_SUPPORTED && !SOC_SDMMC_USE_GPIO_MATRIX
#if __has_include("hal/sdmmc_periph.h")
#include "hal/sdmmc_periph.h"
#else
#include "soc/sdmmc_periph.h"
#endif
#endif
#if SOC_ADC_SUPPORTED
#if __has_include("hal/adc_periph.h")
#include "hal/adc_periph.h"
#else
#include "soc/adc_periph.h"
#endif
#endif
/* IDF 6 moves peripheral counts from SoC capabilities to HAL. */
#if SOC_I2S_SUPPORTED
#ifdef SOC_I2S_NUM
#define BRIDGE_I2S_COUNT SOC_I2S_NUM
#else
#include "hal/i2s_ll.h"
#define BRIDGE_I2S_COUNT I2S_LL_GET(INST_NUM)
#endif
#endif
#if SOC_ADC_SUPPORTED
#ifdef SOC_ADC_CHANNEL_NUM
#define BRIDGE_ADC_CHANNEL_COUNT(unit) SOC_ADC_CHANNEL_NUM(unit)
#else
#define BRIDGE_ADC_CHANNEL_COUNT(unit) ADC_LL_CHANNEL_NUM(unit)
#endif
#endif

static const char *TAG = "claw_hw_bridge";

/* SDMMC: slot, clock, command and up to eight data pins. */
#define RESOURCE_MAX_KEYS 11

typedef struct {
    char (*keys)[CLAW_HW_KEY_SIZE];
    claw_hw_resource_config_t *resources;
    const char **names;
    size_t count;
    size_t capacity;
} bridge_catalog_t;

/* Temporary batch storage; this does not limit the registry. */
static size_t physical_catalog_size(void)
{
    const app_claw_hw_board_config_t *board = &app_claw_hw_generated_board;
    size_t capacity = GPIO_NUM_MAX + SOC_SPI_PERIPH_NUM + board->group_count;
#if SOC_I2C_SUPPORTED
    capacity += SOC_I2C_NUM;
#endif
#if SOC_UART_SUPPORTED
    capacity += SOC_UART_NUM;
#endif
#if SOC_I2S_SUPPORTED
    capacity += BRIDGE_I2S_COUNT;
#endif
#if SOC_ADC_SUPPORTED
    for (int unit = 0; unit < SOC_ADC_PERIPH_NUM; ++unit) capacity += BRIDGE_ADC_CHANNEL_COUNT(unit) + 1;
#endif
#if SOC_LEDC_SUPPORTED
    capacity += LEDC_SPEED_MODE_MAX * (LEDC_TIMER_MAX + LEDC_CHANNEL_MAX);
#endif
    for (size_t i = 0; i < board->group_count; ++i) capacity += board->groups[i].resource_count * RESOURCE_MAX_KEYS;
    return capacity;
}

static void catalog_delete(bridge_catalog_t *catalog)
{
    if (!catalog) return;
    free(catalog->names);
    free(catalog->resources);
    free(catalog->keys);
    free(catalog);
}

static bridge_catalog_t *catalog_create(size_t capacity)
{
    bridge_catalog_t *catalog = calloc(1, sizeof(*catalog));
    if (!catalog) return NULL;
    catalog->capacity = capacity ? capacity : 1;
    catalog->keys = calloc(catalog->capacity, sizeof(*catalog->keys));
    catalog->resources = calloc(catalog->capacity, sizeof(*catalog->resources));
    catalog->names = calloc(catalog->capacity, sizeof(*catalog->names));
    if (!catalog->keys || !catalog->resources || !catalog->names) {
        catalog_delete(catalog);
        return NULL;
    }
    return catalog;
}

static esp_err_t catalog_add(bridge_catalog_t *catalog, const char *key, claw_hw_policy_t policy)
{
    if (!key || !key[0] || strlen(key) >= CLAW_HW_KEY_SIZE) return ESP_ERR_INVALID_ARG;
    size_t index = 0;
    while (index < catalog->count && strcmp(catalog->keys[index], key) != 0) ++index;
    if (index == catalog->count) {
        if (index == catalog->capacity) return ESP_ERR_INVALID_SIZE;
        strcpy(catalog->keys[index], key);
        catalog->resources[index] = (claw_hw_resource_config_t){catalog->keys[index], policy};
        catalog->names[index] = catalog->keys[index];
        ++catalog->count;
    } else if (catalog->resources[index].policy != policy) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t catalog_add_resource(bridge_catalog_t *catalog, app_claw_hw_resource_kind_t kind, int a, int b)
{
    char key[CLAW_HW_KEY_SIZE];
    const char *result = NULL;
    switch (kind) {
    case APP_CLAW_HW_GPIO:
        if (a < 0 || a >= GPIO_NUM_MAX || !GPIO_IS_VALID_GPIO(a)) return ESP_ERR_INVALID_ARG;
        result = claw_hw_key_gpio(key, sizeof(key), a); break;
    case APP_CLAW_HW_I2C_BUS: result = claw_hw_key_i2c_controller(key, sizeof(key), a); break;
    case APP_CLAW_HW_I2C_ADDR: result = claw_hw_key_i2c_addr(key, sizeof(key), a, b); break;
    case APP_CLAW_HW_I2S_BUS: result = claw_hw_key_i2s_bus(key, sizeof(key), a); break;
    case APP_CLAW_HW_SPI_BUS: result = claw_hw_key_spi_bus(key, sizeof(key), a); break;
    case APP_CLAW_HW_UART_BUS: result = claw_hw_key_uart_bus(key, sizeof(key), a); break;
    case APP_CLAW_HW_ADC_UNIT: result = claw_hw_key_adc_unit(key, sizeof(key), a); break;
    case APP_CLAW_HW_ADC_CHANNEL: result = claw_hw_key_adc_channel(key, sizeof(key), a, b); break;
    case APP_CLAW_HW_LEDC_TIMER: result = claw_hw_key_ledc_timer(key, sizeof(key), a, b); break;
    case APP_CLAW_HW_LEDC_CHANNEL: result = claw_hw_key_ledc_channel(key, sizeof(key), a, b); break;
    case APP_CLAW_HW_SDMMC_SLOT: result = claw_hw_key_sdmmc_slot(key, sizeof(key), a); break;
    case APP_CLAW_HW_DSI_BUS: result = claw_hw_key_dsi_bus(key, sizeof(key), a); break;
    case APP_CLAW_HW_LDO_CHANNEL: result = claw_hw_key_ldo_channel(key, sizeof(key), a); break;
    default: return ESP_ERR_INVALID_ARG;
    }
    return catalog_add(catalog, result, CLAW_HW_EXCLUSIVE);
}

static esp_err_t catalog_add_device(bridge_catalog_t *catalog, const app_claw_hw_group_t *group)
{
    if (!group->device) return ESP_OK;
    char key[CLAW_HW_KEY_SIZE];
    return catalog_add(catalog, claw_hw_key_device(key, sizeof(key), group->name), CLAW_HW_EXCLUSIVE);
}

#define ADD(kind, a, b) do { \
    esp_err_t err = catalog_add_resource(catalog, APP_CLAW_HW_##kind, a, b); \
    if (err != ESP_OK) return err; \
} while (0)

static esp_err_t add_board_resource(bridge_catalog_t *catalog, const app_claw_hw_resource_t *resource)
{
    esp_err_t err = catalog_add_resource(catalog, resource->kind, resource->a, resource->b);
    if (err != ESP_OK) return err;
#if SOC_ADC_SUPPORTED
    if (resource->kind == APP_CLAW_HW_ADC_CHANNEL) {
        int unit = resource->a, channel = resource->b;
        if (unit < 0 || unit >= SOC_ADC_PERIPH_NUM || channel < 0 || channel >= BRIDGE_ADC_CHANNEL_COUNT(unit)) return ESP_ERR_INVALID_ARG;
        ADD(GPIO, adc_channel_io_map[unit][channel], 0);
    }
#endif
#if SOC_SDMMC_HOST_SUPPORTED && !SOC_SDMMC_USE_GPIO_MATRIX
    if (resource->kind == APP_CLAW_HW_SDMMC_SLOT) {
        int slot = resource->a, width = resource->b;
        if (slot < 0 || slot >= SOC_SDMMC_NUM_SLOTS || (width != 1 && width != 4 && width != 8)) return ESP_ERR_INVALID_ARG;
        if (width > sdmmc_slot_info[slot].width) return ESP_ERR_INVALID_ARG;
        const sdmmc_slot_io_info_t *pins = &sdmmc_slot_gpio_num[slot];
        const int data[] = {pins->d0, pins->d1, pins->d2, pins->d3, pins->d4, pins->d5, pins->d6, pins->d7};
        ADD(GPIO, pins->clk, 0);
        ADD(GPIO, pins->cmd, 0);
        for (int i = 0; i < width; ++i) ADD(GPIO, data[i], 0);
    }
#endif
    return ESP_OK;
}

static esp_err_t build_catalog(bridge_catalog_t *catalog)
{
    for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
        if (GPIO_IS_VALID_GPIO(pin)) ADD(GPIO, pin, 0);
    }
    for (int host = SPI2_HOST; host < SOC_SPI_PERIPH_NUM; ++host) ADD(SPI_BUS, host, 0);
#if SOC_I2C_SUPPORTED
    for (int port = 0; port < SOC_I2C_NUM; ++port) {
        ADD(I2C_BUS, port, 0);
    }
#endif
#if SOC_UART_SUPPORTED
    for (int port = 0; port < SOC_UART_NUM; ++port) ADD(UART_BUS, port, 0);
#endif
#if SOC_I2S_SUPPORTED
    for (int port = 0; port < BRIDGE_I2S_COUNT; ++port) ADD(I2S_BUS, port, 0);
#endif
#if SOC_ADC_SUPPORTED
    for (int unit = 0; unit < SOC_ADC_PERIPH_NUM; ++unit) {
        ADD(ADC_UNIT, unit, 0);
        for (int channel = 0; channel < BRIDGE_ADC_CHANNEL_COUNT(unit); ++channel) ADD(ADC_CHANNEL, unit, channel);
    }
#endif
#if SOC_LEDC_SUPPORTED
    for (int mode = 0; mode < LEDC_SPEED_MODE_MAX; ++mode) {
        for (int timer = 0; timer < LEDC_TIMER_MAX; ++timer) ADD(LEDC_TIMER, mode, timer);
        for (int channel = 0; channel < LEDC_CHANNEL_MAX; ++channel) ADD(LEDC_CHANNEL, mode, channel);
    }
#endif
    const app_claw_hw_board_config_t *board = &app_claw_hw_generated_board;
    for (size_t i = 0; i < board->group_count; ++i) {
        const app_claw_hw_group_t *group = &board->groups[i];
        for (size_t j = 0; j < group->resource_count; ++j) {
            /* I2C addresses are checked when the device claims them. */
            if (group->resources[j].kind == APP_CLAW_HW_I2C_ADDR) continue;
            esp_err_t err = add_board_resource(catalog, &group->resources[j]);
            if (err != ESP_OK) return err;
        }
        esp_err_t err = catalog_add_device(catalog, group);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
#undef ADD

typedef struct {
    claw_hw_claim_id_t claim;
    size_t users;
} board_usage_t;

typedef struct {
    claw_hw_registry_handle_t registry;
    board_usage_t usage[];
} board_bridge_t;

/* One board manager per firmware; its bridge lives until reboot. */
static board_bridge_t *s_bridge;

esp_err_t app_claw_hw_bridge_init(claw_hw_registry_handle_t registry)
{
    if (!registry || s_bridge) return ESP_ERR_INVALID_STATE;
    board_bridge_t *bridge = calloc(1, sizeof(*bridge) + app_claw_hw_generated_board.group_count * sizeof(board_usage_t));
    if (!bridge) return ESP_ERR_NO_MEM;
    bridge_catalog_t *catalog = catalog_create(physical_catalog_size());
    if (!catalog) { free(bridge); return ESP_ERR_NO_MEM; }
    esp_err_t err = build_catalog(catalog);
    if (err == ESP_OK) {
        err = claw_hw_registry_register(registry, catalog->resources, catalog->count);
        if (err == ESP_OK) ESP_LOGI(TAG, "Registered %u resources", (unsigned)catalog->count);
    }
    catalog_delete(catalog);
    if (err != ESP_OK) { free(bridge); return err; }
    bridge->registry = registry;
    s_bridge = bridge;
    return ESP_OK;
}

static const app_claw_hw_group_t *find_group(const char *name, bool device)
{
    const app_claw_hw_board_config_t *board = &app_claw_hw_generated_board;
    for (size_t i = 0; i < board->group_count; ++i) {
        const app_claw_hw_group_t *group = &board->groups[i];
        if (group->device == device && strcmp(group->name, name) == 0) return group;
    }
    return NULL;
}

static esp_err_t claim_group(const app_claw_hw_group_t *group)
{
    board_usage_t *usage = &s_bridge->usage[group->group];
    if (usage->users) return ESP_OK;
    bridge_catalog_t *catalog = catalog_create(group->resource_count * RESOURCE_MAX_KEYS + 1);
    if (!catalog) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < group->resource_count && err == ESP_OK; ++i) err = add_board_resource(catalog, &group->resources[i]);
    if (err == ESP_OK && group->board_owned) err = catalog_add_device(catalog, group);
    if (err == ESP_OK && catalog->count) {
        const claw_hw_claim_config_t claim = {.owner = group->name, .resources = catalog->names, .resource_count = catalog->count};
        err = claw_hw_registry_claim(s_bridge->registry, &claim, &usage->claim);
    }
    catalog_delete(catalog);
    return err;
}

static void release_group(board_usage_t *usage)
{
    if (!usage->claim) return;
    esp_err_t err = claw_hw_registry_release(s_bridge->registry, usage->claim);
    if (err == ESP_OK) usage->claim = CLAW_HW_CLAIM_INVALID;
    else ESP_LOGE(TAG, "Release failed: %s", esp_err_to_name(err));
}

/* Called only where Board Manager creates or destroys the actual driver. */
esp_err_t app_claw_hw_driver_init(const char *name, bool device, esp_err_t (*init)(void *, int, void **), void *config, int size, void **handle)
{
    if (!s_bridge) return ESP_ERR_INVALID_STATE;
    const app_claw_hw_group_t *group = find_group(name, device);
    if (!group) return init(config, size, handle); /* Custom devices remain outside the standard map. */
    esp_err_t err = claim_group(group);
    if (err != ESP_OK) return err;
    board_usage_t *usage = &s_bridge->usage[group->group];
    err = init(config, size, handle);
    if (err == ESP_OK) ++usage->users;
    else if (!usage->users) release_group(usage);
    return err;
}

esp_err_t app_claw_hw_driver_deinit(const char *name, bool device, esp_err_t (*deinit)(void *), void *handle)
{
    if (!s_bridge) return ESP_ERR_INVALID_STATE;
    const app_claw_hw_group_t *group = find_group(name, device);
    if (group && group->device && !group->board_owned) {
        char key[CLAW_HW_KEY_SIZE];
        const char *access = claw_hw_key_device(key, sizeof(key), group->name);
        if (!access) return ESP_ERR_INVALID_ARG;
        size_t count = 0;
        claw_hw_entry_t entry;
        esp_err_t err = claw_hw_registry_query(s_bridge->registry, access, &entry, 1, &count);
        if (err == ESP_ERR_INVALID_SIZE || (err == ESP_OK && entry.users)) return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = deinit(handle);
    if (err == ESP_OK && group) {
        board_usage_t *usage = &s_bridge->usage[group->group];
        if (usage->users && --usage->users == 0) release_group(usage);
    }
    return err;
}
