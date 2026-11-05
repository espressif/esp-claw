/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_hw_registry.h"

#include <stdbool.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    char key[CLAW_HW_KEY_SIZE];
    claw_hw_policy_t policy;
    size_t users;
    size_t parent; /* Parent slot + 1 for dynamic addresses; zero for registered resources. */
} resource_t;

typedef struct {
    claw_hw_claim_id_t id;
    char owner[CLAW_HW_OWNER_SIZE];
} claim_t;

typedef struct {
    bool used;
    size_t resource;
    size_t claim;
} binding_t;

struct claw_hw_registry_t {
    SemaphoreHandle_t lock;
    size_t alloc_resources;
    size_t alloc_claims;
    size_t alloc_bindings;
    resource_t *resources;
    claim_t *claims;
    binding_t *bindings;
    claw_hw_claim_id_t last_id;
};

static const char *TAG = "hw_registry";
#define I2C_ADDRESS_PREFIX "i2c_addr:"
#define TABLE_GROWTH_STEP 16

static bool valid_text(const char *text, size_t size)
{
    return text && text[0] && strnlen(text, size) < size;
}

static size_t find_resource(claw_hw_registry_handle_t h, const char *key)
{
    for (size_t i = 0; i < h->alloc_resources; ++i) {
        if (strcmp(h->resources[i].key, key) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static esp_err_t i2c_parent(claw_hw_registry_handle_t h, const char *key, size_t *parent)
{
    if (strncmp(key, I2C_ADDRESS_PREFIX, sizeof(I2C_ADDRESS_PREFIX) - 1) != 0) return ESP_ERR_NOT_FOUND;
    const char *number = key + sizeof(I2C_ADDRESS_PREFIX) - 1;
    char *end;
    unsigned long port = strtoul(number, &end, 10);
    if (end == number || *end != ':' || port > INT_MAX) return ESP_ERR_INVALID_ARG;
    number = end + 1;
    unsigned long address = strtoul(number, &end, 16);
    if (end == number || *end || address > 0x7f) return ESP_ERR_INVALID_ARG;
    char canonical[CLAW_HW_KEY_SIZE];
    claw_hw_key_i2c_addr(canonical, sizeof(canonical), (int)port, (int)address);
    if (strcmp(key, canonical) != 0) return ESP_ERR_INVALID_ARG;
    claw_hw_key_i2c_controller(canonical, sizeof(canonical), (int)port);
    size_t slot = find_resource(h, canonical);
    if (slot == SIZE_MAX) return ESP_ERR_NOT_FOUND;
    *parent = slot + 1;
    return ESP_OK;
}

static esp_err_t validate_keys(const char *const *keys, size_t count)
{
    if (!keys || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!valid_text(keys[i], CLAW_HW_KEY_SIZE)) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(keys[i], keys[j]) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t unlock(claw_hw_registry_handle_t h, esp_err_t result)
{
    xSemaphoreGive(h->lock);
    if (result == ESP_ERR_NO_MEM) {
        ESP_LOGE(TAG, "Registry growth failed");
    }
    return result;
}

/* Called under the registry lock; indices survive reallocations. */
static void *grow_table(void *table, size_t *allocated, size_t used, size_t extra, size_t item_size)
{
    size_t limit = SIZE_MAX / item_size;
    if (used > limit || extra > limit - used) return NULL;
    size_t needed = used + extra;
    if (needed <= *allocated) return table;
    size_t padding = (TABLE_GROWTH_STEP - needed % TABLE_GROWTH_STEP) % TABLE_GROWTH_STEP;
    size_t slots = needed + (padding < limit - needed ? padding : limit - needed);
    void *grown = realloc(table, slots * item_size);
    if (!grown) return NULL;
    memset((char *)grown + *allocated * item_size, 0, (slots - *allocated) * item_size);
    *allocated = slots;
    return grown;
}

esp_err_t claw_hw_registry_create(claw_hw_registry_handle_t *ret_handle)
{
    if (ret_handle) {
        *ret_handle = NULL;
    }
    if (!ret_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    claw_hw_registry_handle_t h = calloc(1, sizeof(*h));
    if (!h) {
        ESP_LOGE(TAG, "Registry allocation failed");
        return ESP_ERR_NO_MEM;
    }
    h->lock = xSemaphoreCreateMutex();
    if (!h->lock) {
        free(h);
        ESP_LOGE(TAG, "Registry allocation failed");
        return ESP_ERR_NO_MEM;
    }
    *ret_handle = h;
    return ESP_OK;
}

esp_err_t claw_hw_registry_delete(claw_hw_registry_handle_t h)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    for (size_t i = 0; i < h->alloc_resources; ++i) {
        if (h->resources[i].key[0]) {
            return unlock(h, ESP_ERR_INVALID_STATE);
        }
    }
    for (size_t i = 0; i < h->alloc_claims; ++i) {
        if (h->claims[i].id) {
            return unlock(h, ESP_ERR_INVALID_STATE);
        }
    }
    xSemaphoreGive(h->lock);
    vSemaphoreDelete(h->lock);
    free(h->bindings);
    free(h->claims);
    free(h->resources);
    free(h);
    return ESP_OK;
}

esp_err_t claw_hw_registry_register(claw_hw_registry_handle_t h, const claw_hw_resource_config_t *resources, size_t count)
{
    if (!h || !resources || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!valid_text(resources[i].resource, CLAW_HW_KEY_SIZE) ||
            strncmp(resources[i].resource, I2C_ADDRESS_PREFIX, sizeof(I2C_ADDRESS_PREFIX) - 1) == 0 ||
            (resources[i].policy != CLAW_HW_EXCLUSIVE && resources[i].policy != CLAW_HW_SHARED)) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(resources[i].resource, resources[j].resource) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    for (size_t i = 0; i < count; ++i) {
        if (find_resource(h, resources[i].resource) != SIZE_MAX) {
            return unlock(h, ESP_ERR_INVALID_STATE);
        }
    }
    size_t available = 0;
    for (size_t i = 0; i < h->alloc_resources; ++i) {
        available += !h->resources[i].key[0];
    }
    size_t used = h->alloc_resources - available;
    resource_t *grown = grow_table(h->resources, &h->alloc_resources, used, count, sizeof(resource_t));
    if (!grown) return unlock(h, ESP_ERR_NO_MEM);
    h->resources = grown;
    size_t slot = 0;
    for (size_t i = 0; i < count; ++i) {
        while (h->resources[slot].key[0]) {
            ++slot;
        }
        strcpy(h->resources[slot].key, resources[i].resource);
        h->resources[slot].policy = resources[i].policy;
    }
    return unlock(h, ESP_OK);
}

esp_err_t claw_hw_registry_unregister(claw_hw_registry_handle_t h, const char *const *resources, size_t count)
{
    if (!h || validate_keys(resources, count) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    for (size_t i = 0; i < count; ++i) {
        size_t slot = find_resource(h, resources[i]);
        if (slot == SIZE_MAX) {
            return unlock(h, ESP_ERR_NOT_FOUND);
        }
        if (h->resources[slot].users) {
            return unlock(h, ESP_ERR_INVALID_STATE);
        }
        for (size_t j = 0; j < h->alloc_resources; ++j) {
            if (h->resources[j].parent == slot + 1) return unlock(h, ESP_ERR_INVALID_STATE);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        memset(&h->resources[find_resource(h, resources[i])], 0, sizeof(resource_t));
    }
    return unlock(h, ESP_OK);
}

typedef struct {
    char key[CLAW_HW_KEY_SIZE];
    char owner[CLAW_HW_OWNER_SIZE];
} conflict_t;

_Static_assert(sizeof(conflict_t) <= 128, "Conflict record exceeds stack budget");

static esp_err_t check_claim(claw_hw_registry_handle_t h, const claw_hw_claim_config_t *config, conflict_t *conflict, size_t *new_resources)
{
    /* Validate every key before checking conflicts or mutating any table. */
    for (size_t i = 0; i < config->resource_count; ++i) {
        if (find_resource(h, config->resources[i]) == SIZE_MAX) {
            size_t parent;
            esp_err_t err = i2c_parent(h, config->resources[i], &parent);
            if (err != ESP_OK) return err;
            ++*new_resources;
        }
    }
    for (size_t i = 0; i < config->resource_count; ++i) {
        size_t slot = find_resource(h, config->resources[i]);
        if (slot == SIZE_MAX) continue;
        resource_t *resource = &h->resources[slot];
        if (resource->policy == CLAW_HW_EXCLUSIVE && resource->users) {
            strcpy(conflict->key, resource->key);
            for (size_t j = 0; j < h->alloc_bindings; ++j) {
                if (h->bindings[j].used && h->bindings[j].resource == slot) {
                    strcpy(conflict->owner, h->claims[h->bindings[j].claim].owner);
                    break;
                }
            }
            return CLAW_HW_ERR_CONFLICT;
        }
    }
    return ESP_OK;
}

esp_err_t claw_hw_registry_claim(claw_hw_registry_handle_t h, const claw_hw_claim_config_t *config, claw_hw_claim_id_t *ret_id)
{
    if (ret_id) {
        *ret_id = CLAW_HW_CLAIM_INVALID;
    }
    if (!h || !config || !ret_id || !valid_text(config->owner, CLAW_HW_OWNER_SIZE) ||
        validate_keys(config->resources, config->resource_count) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    conflict_t conflict = {0};
    size_t new_resources = 0;
    esp_err_t err = check_claim(h, config, &conflict, &new_resources);
    if (err != ESP_OK) {
        xSemaphoreGive(h->lock);
        if (err == CLAW_HW_ERR_CONFLICT) {
            ESP_LOGW(TAG, "Conflict %s: owner=%s requester=%s", conflict.key, conflict.owner, config->owner);
        }
        return err;
    }
    size_t claim = 0;
    while (claim < h->alloc_claims && h->claims[claim].id) {
        ++claim;
    }
    size_t available = 0;
    for (size_t i = 0; i < h->alloc_bindings; ++i) {
        available += !h->bindings[i].used;
    }
    size_t free_resources = 0;
    for (size_t i = 0; i < h->alloc_resources; ++i) free_resources += !h->resources[i].key[0];
    size_t used_resources = h->alloc_resources - free_resources;
    size_t used_bindings = h->alloc_bindings - available;
    if (h->last_id == UINT64_MAX) {
        xSemaphoreGive(h->lock);
        ESP_LOGE(TAG, "Claim ID space exhausted");
        return ESP_ERR_INVALID_STATE;
    }
    /* Grow all tables before publishing resources, claims or bindings. Indices remain stable. */
    resource_t *resources = grow_table(h->resources, &h->alloc_resources, used_resources, new_resources, sizeof(resource_t));
    if (!resources) return unlock(h, ESP_ERR_NO_MEM);
    h->resources = resources;
    claim_t *claims = grow_table(h->claims, &h->alloc_claims, claim, 1, sizeof(claim_t));
    if (!claims) return unlock(h, ESP_ERR_NO_MEM);
    h->claims = claims;
    binding_t *bindings = grow_table(h->bindings, &h->alloc_bindings, used_bindings, config->resource_count, sizeof(binding_t));
    if (!bindings) return unlock(h, ESP_ERR_NO_MEM);
    h->bindings = bindings;
    h->claims[claim].id = ++h->last_id;
    strcpy(h->claims[claim].owner, config->owner);
    size_t binding = 0, resource_slot = 0;
    for (size_t i = 0; i < config->resource_count; ++i) {
        while (h->bindings[binding].used) {
            ++binding;
        }
        size_t resource = find_resource(h, config->resources[i]);
        if (resource == SIZE_MAX) {
            while (h->resources[resource_slot].key[0]) ++resource_slot;
            resource = resource_slot;
            resource_t *entry = &h->resources[resource];
            /* The parent and capacity were validated under this same lock. */
            i2c_parent(h, config->resources[i], &entry->parent);
            strcpy(entry->key, config->resources[i]);
            entry->policy = CLAW_HW_EXCLUSIVE;
        }
        h->bindings[binding] = (binding_t){.used = true, .resource = resource, .claim = claim};
        ++h->resources[resource].users;
    }
    *ret_id = h->claims[claim].id;
    return unlock(h, ESP_OK);
}

esp_err_t claw_hw_registry_release(claw_hw_registry_handle_t h, claw_hw_claim_id_t id)
{
    if (!h || !id) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    size_t claim = 0;
    while (claim < h->alloc_claims && h->claims[claim].id != id) {
        ++claim;
    }
    if (claim == h->alloc_claims) {
        return unlock(h, ESP_ERR_NOT_FOUND);
    }
    for (size_t i = 0; i < h->alloc_bindings; ++i) {
        binding_t *binding = &h->bindings[i];
        if (binding->used && binding->claim == claim) {
            resource_t *resource = &h->resources[binding->resource];
            if (--resource->users == 0 && resource->parent) memset(resource, 0, sizeof(*resource));
            binding->used = false;
        }
    }
    memset(&h->claims[claim], 0, sizeof(claim_t));
    return unlock(h, ESP_OK);
}

static void copy_entry(claw_hw_entry_t *entry, const resource_t *resource, const claim_t *claim)
{
    memset(entry, 0, sizeof(*entry));
    strcpy(entry->resource, resource->key);
    entry->policy = resource->policy;
    entry->users = resource->users;
    if (claim) {
        entry->claim_id = claim->id;
        strcpy(entry->owner, claim->owner);
    }
}

esp_err_t claw_hw_registry_query(claw_hw_registry_handle_t h, const char *key, claw_hw_entry_t *entries, size_t capacity, size_t *count)
{
    if (count) {
        *count = 0;
    }
    if (!h || !count || (!entries && capacity) || (key && !valid_text(key, CLAW_HW_KEY_SIZE))) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    size_t selected = key ? find_resource(h, key) : SIZE_MAX;
    if (key && selected == SIZE_MAX) {
        return unlock(h, ESP_ERR_NOT_FOUND);
    }
    for (size_t i = 0; i < h->alloc_resources; ++i) {
        resource_t *resource = &h->resources[i];
        if (resource->key[0] && (!key || i == selected)) {
            *count += resource->users ? resource->users : 1;
        }
    }
    if (capacity < *count) {
        return unlock(h, ESP_ERR_INVALID_SIZE);
    }
    size_t row = 0;
    for (size_t i = 0; i < h->alloc_resources; ++i) {
        resource_t *resource = &h->resources[i];
        if (!resource->key[0] || (key && i != selected)) {
            continue;
        }
        if (!resource->users) {
            copy_entry(&entries[row++], resource, NULL);
            continue;
        }
        for (size_t j = 0; j < h->alloc_bindings; ++j) {
            binding_t *binding = &h->bindings[j];
            if (binding->used && binding->resource == i) {
                copy_entry(&entries[row++], resource, &h->claims[binding->claim]);
            }
        }
    }
    return unlock(h, ESP_OK);
}

static const char *format_key(char *buf, size_t size, bool valid, const char *format, ...)
{
    if (!buf || !size) {
        return NULL;
    }
    buf[0] = '\0';
    if (!valid) {
        return NULL;
    }
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buf, size, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= size || length >= CLAW_HW_KEY_SIZE) {
        buf[0] = '\0';
        return NULL;
    }
    return buf;
}

const char *claw_hw_key_gpio(char *buf, size_t size, int pin)
{
    return format_key(buf, size, pin >= 0, "gpio:%d", pin);
}

const char *claw_hw_key_uart_bus(char *buf, size_t size, int port)
{
    return format_key(buf, size, port >= 0, "uart_bus:%d", port);
}

const char *claw_hw_key_i2c_controller(char *buf, size_t size, int port)
{
    return format_key(buf, size, port >= 0, "i2c_bus:%d", port);
}

const char *claw_hw_key_i2c_addr(char *buf, size_t size, int port, int address)
{
    return format_key(buf, size, port >= 0 && address >= 0 && address <= 0x7f, "i2c_addr:%d:%02x", port, (unsigned)address);
}

const char *claw_hw_key_spi_bus(char *buf, size_t size, int host)
{
    return format_key(buf, size, host >= 0, "spi_bus:%d", host);
}

const char *claw_hw_key_i2s_bus(char *buf, size_t size, int port)
{
    return format_key(buf, size, port >= 0, "i2s_bus:%d", port);
}

const char *claw_hw_key_adc_unit(char *buf, size_t size, int unit)
{
    return format_key(buf, size, unit >= 0, "adc_unit:%d", unit);
}

const char *claw_hw_key_adc_channel(char *buf, size_t size, int unit, int channel)
{
    return format_key(buf, size, unit >= 0 && channel >= 0, "adc_channel:%d:%d", unit, channel);
}

const char *claw_hw_key_ledc_timer(char *buf, size_t size, int mode, int timer)
{
    return format_key(buf, size, mode >= 0 && timer >= 0, "ledc_timer:%d:%d", mode, timer);
}

const char *claw_hw_key_ledc_channel(char *buf, size_t size, int mode, int channel)
{
    return format_key(buf, size, mode >= 0 && channel >= 0, "ledc_channel:%d:%d", mode, channel);
}

const char *claw_hw_key_device(char *buf, size_t size, const char *name)
{
    return format_key(buf, size, valid_text(name, CLAW_HW_KEY_SIZE), "device:%s", name);
}

const char *claw_hw_key_sdmmc_slot(char *buf, size_t size, int slot)
{
    return format_key(buf, size, slot >= 0, "sdmmc_slot:%d", slot);
}

const char *claw_hw_key_dsi_bus(char *buf, size_t size, int bus)
{
    return format_key(buf, size, bus >= 0, "dsi_bus:%d", bus);
}

const char *claw_hw_key_ldo_channel(char *buf, size_t size, int channel)
{
    return format_key(buf, size, channel >= 0, "ldo_channel:%d", channel);
}
