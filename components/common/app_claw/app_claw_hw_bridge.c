/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw_hw_bridge.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"

#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_board_manager_includes.h"

static const char *TAG = "app_claw_hw_bridge";

/* type=custom devices have a per-YAML-generated struct layout that the
 * bridge cannot introspect, so it just logs and skips. */
#define BRIDGE_TYPE_CUSTOM      "custom"
#define BRIDGE_OWNER_TAG_MAX    96
#define BRIDGE_KEY_BUF          64

static inline bool bridge_gpio_valid(int pin)
{
    return pin >= 0 && pin < 256;
}

static inline bool bridge_i2c_addr8_valid(int addr8)
{
    /* esp_board_manager stores 8-bit left-shifted addresses. */
    return addr8 > 0 && addr8 <= 0xFF;
}

/* Leases held for the app's full lifetime so board-owned resources stay
 * blocked from raw Lua drivers. */
typedef struct bridge_lease_node {
    claw_hw_lease_handle_t   lease;
    struct bridge_lease_node *next;
} bridge_lease_node_t;

static bridge_lease_node_t *s_lease_list;
static bool                 s_bridge_registered;
static SemaphoreHandle_t    s_bridge_mutex;
static portMUX_TYPE         s_bridge_init_spinlock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t bridge_ensure_mutex(void)
{
    portENTER_CRITICAL(&s_bridge_init_spinlock);
    bool have = (s_bridge_mutex != NULL);
    portEXIT_CRITICAL(&s_bridge_init_spinlock);
    if (have) {
        return ESP_OK;
    }
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    if (m == NULL) {
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_bridge_init_spinlock);
    if (s_bridge_mutex == NULL) {
        s_bridge_mutex = m;
        m = NULL;
    }
    portEXIT_CRITICAL(&s_bridge_init_spinlock);
    if (m != NULL) {
        vSemaphoreDelete(m);
    }
    return ESP_OK;
}

static void bridge_lock(void)
{
    xSemaphoreTake(s_bridge_mutex, portMAX_DELAY);
}

static void bridge_unlock(void)
{
    xSemaphoreGive(s_bridge_mutex);
}

static void bridge_record_lease(claw_hw_lease_handle_t lease)
{
    if (lease == NULL) {
        return;
    }
    bridge_lease_node_t *node = calloc(1, sizeof(*node));
    if (node == NULL) {
        /* Registry still tracks the row; we just cannot walk it locally. */
        ESP_LOGW(TAG, "OOM recording bridge lease; entry still active in registry");
        return;
    }
    node->lease = lease;
    bridge_lock();
    node->next = s_lease_list;
    s_lease_list = node;
    bridge_unlock();
}

/* Peripheral introspection: each helper returns false when the named
 * peripheral cannot be resolved so callers can skip with a DEBUG log. */

#ifdef CONFIG_ESP_BOARD_PERIPH_I2C_SUPPORT
static bool bridge_periph_i2c_port(const char *name, int *out_port)
{
    if (name == NULL || out_port == NULL) {
        return false;
    }
    void *cfg = NULL;
    if (esp_board_manager_get_periph_config(name, &cfg) != ESP_OK || cfg == NULL) {
        return false;
    }
    const i2c_master_bus_config_t *bus = cfg;
    *out_port = (int)bus->i2c_port;
    return true;
}
#endif

#ifdef CONFIG_ESP_BOARD_PERIPH_SPI_SUPPORT
static bool bridge_periph_spi_host(const char *name, int *out_host)
{
    if (name == NULL || out_host == NULL) {
        return false;
    }
    void *cfg = NULL;
    if (esp_board_manager_get_periph_config(name, &cfg) != ESP_OK || cfg == NULL) {
        return false;
    }
    const periph_spi_config_t *spi = cfg;
    *out_host = (int)spi->spi_port;
    return true;
}
#endif

#ifdef CONFIG_ESP_BOARD_PERIPH_I2S_SUPPORT
static bool bridge_periph_i2s_port_dir(const char *name, int *out_port, bool *out_tx, bool *out_rx)
{
    if (name == NULL || out_port == NULL || out_tx == NULL || out_rx == NULL) {
        return false;
    }
    void *cfg = NULL;
    if (esp_board_manager_get_periph_config(name, &cfg) != ESP_OK || cfg == NULL) {
        return false;
    }
    const periph_i2s_config_t *i2s = cfg;
    *out_port = i2s->port;
    *out_tx = (i2s->direction & I2S_DIR_TX) != 0;
    *out_rx = (i2s->direction & I2S_DIR_RX) != 0;
    return true;
}
#endif

#ifdef CONFIG_ESP_BOARD_PERIPH_GPIO_SUPPORT
static bool bridge_periph_gpio_mask(const char *name, uint64_t *out_mask)
{
    if (name == NULL || out_mask == NULL) {
        return false;
    }
    void *cfg = NULL;
    if (esp_board_manager_get_periph_config(name, &cfg) != ESP_OK || cfg == NULL) {
        return false;
    }
    const periph_gpio_config_t *gpio = cfg;
    *out_mask = gpio->gpio_config.pin_bit_mask;
    return true;
}
#endif

#if defined(CONFIG_ESP_BOARD_PERIPH_LEDC_SUPPORT) && defined(CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT)
static bool bridge_periph_ledc_gpio(const char *name, int *out_pin)
{
    if (name == NULL || out_pin == NULL) {
        return false;
    }
    void *cfg = NULL;
    if (esp_board_manager_get_periph_config(name, &cfg) != ESP_OK || cfg == NULL) {
        return false;
    }
    const periph_ledc_config_t *ledc = cfg;
    *out_pin = ledc->gpio_num;
    return true;
}
#endif

static void bridge_claim_resource(const char *device_name,
                                  const char *owner_tag,
                                  const char *resource,
                                  claw_hw_mode_t mode,
                                  size_t *registered_count)
{
    if (resource == NULL || resource[0] == '\0') {
        return;
    }

    /* Sibling board devices legitimately share physical wires (a duplex
     * codec exposes both audio_dac and audio_adc on the same I2C address);
     * skip silently instead of tripping a registry WARN. */
    const char *existing_holder = NULL;
    if (claw_hw_query(resource, &existing_holder) == ESP_OK &&
        existing_holder != NULL &&
        strncmp(existing_holder, "board/", 6) == 0) {
        ESP_LOGD(TAG, "device %s: %s already registered by sibling %s; skipping",
                 device_name != NULL ? device_name : "?",
                 resource, existing_holder);
        return;
    }

    claw_hw_claim_config_t claim = {
        .resource      = resource,
        .owner_tag     = owner_tag,
        .mode          = mode,
        .on_release    = NULL,
        .user_ctx      = NULL,
        .sub_resources = NULL,
    };
    claw_hw_lease_handle_t lease = NULL;
    esp_err_t err = claw_hw_claim(&claim, &lease);
    if (err == ESP_OK) {
        bridge_record_lease(lease);
        if (registered_count != NULL) {
            (*registered_count)++;
        }
        ESP_LOGD(TAG, "registered %s as %s (%s)",
                 resource, owner_tag,
                 mode == CLAW_HW_MODE_EXCLUSIVE ? "EXCLUSIVE" : "SHARED_READ");
        return;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        const char *holder = NULL;
        (void)claw_hw_query(resource, &holder);
        ESP_LOGW(TAG, "device %s: failed to register %s (%s) — held by %s",
                 device_name != NULL ? device_name : "?",
                 resource,
                 mode == CLAW_HW_MODE_EXCLUSIVE ? "EXCLUSIVE" : "SHARED_READ",
                 holder != NULL ? holder : "?");
        return;
    }
    ESP_LOGW(TAG, "device %s: failed to register %s: %s",
             device_name != NULL ? device_name : "?",
             resource, esp_err_to_name(err));
}

static void bridge_claim_gpio(const char *device_name,
                              const char *owner_tag,
                              int pin,
                              size_t *registered_count)
{
    if (!bridge_gpio_valid(pin)) {
        return;
    }
    char buf[BRIDGE_KEY_BUF];
    bridge_claim_resource(device_name, owner_tag,
                          claw_hw_key_gpio(buf, sizeof(buf), pin),
                          CLAW_HW_MODE_EXCLUSIVE,
                          registered_count);
}

#if defined(CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT) || defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUB_I2C_SUPPORT)
static void bridge_claim_i2c(const char *device_name,
                             const char *owner_tag,
                             int port, uint8_t addr7,
                             size_t *registered_count)
{
    char buf[BRIDGE_KEY_BUF];
    bridge_claim_resource(device_name, owner_tag,
                          claw_hw_key_i2c(buf, sizeof(buf), port, addr7),
                          CLAW_HW_MODE_EXCLUSIVE,
                          registered_count);
}
#endif

static void bridge_claim_spi_cs(const char *device_name,
                                const char *owner_tag,
                                int host, int cs_pin,
                                size_t *registered_count)
{
    if (!bridge_gpio_valid(cs_pin)) {
        return;
    }
    char buf[BRIDGE_KEY_BUF];
    bridge_claim_resource(device_name, owner_tag,
                          claw_hw_key_spi(buf, sizeof(buf), host, cs_pin),
                          CLAW_HW_MODE_EXCLUSIVE,
                          registered_count);
}

#if defined(CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT) && defined(CONFIG_ESP_BOARD_PERIPH_I2S_SUPPORT)
static void bridge_claim_i2s(const char *device_name,
                             const char *owner_tag,
                             int port, bool tx,
                             size_t *registered_count)
{
    char buf[BRIDGE_KEY_BUF];
    bridge_claim_resource(device_name, owner_tag,
                          claw_hw_key_i2s(buf, sizeof(buf), port, tx),
                          CLAW_HW_MODE_EXCLUSIVE,
                          registered_count);
}
#endif

#ifdef CONFIG_ESP_BOARD_PERIPH_GPIO_SUPPORT
/* Shared by power_ctrl / gpio_ctrl / button-gpio extractors. */
static size_t bridge_claim_gpio_periph(const char *device_name,
                                       const char *owner_tag,
                                       const char *gpio_periph_name)
{
    size_t registered = 0;
    if (gpio_periph_name == NULL || gpio_periph_name[0] == '\0') {
        ESP_LOGD(TAG, "%s: missing gpio peripheral name",
                 device_name != NULL ? device_name : "?");
        return 0;
    }
    uint64_t mask = 0;
    if (!bridge_periph_gpio_mask(gpio_periph_name, &mask)) {
        ESP_LOGD(TAG, "%s: unresolved gpio peripheral '%s'",
                 device_name != NULL ? device_name : "?", gpio_periph_name);
        return 0;
    }
    for (int pin = 0; mask != 0 && pin < 64; ++pin) {
        if ((mask >> pin) & 0x1ULL) {
            bridge_claim_gpio(device_name, owner_tag, pin, &registered);
        }
    }
    return registered;
}
#endif

/* Per-device-type extractors return the number of resources registered so
 * the caller can flag zero-yield devices. */

#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
static size_t bridge_extract_display_lcd(const char *name, const char *owner_tag)
{
    size_t registered = 0;
    void *raw_cfg = NULL;
    if (esp_board_manager_get_device_config(name, &raw_cfg) != ESP_OK || raw_cfg == NULL) {
        ESP_LOGW(TAG, "display_lcd %s: config unavailable", name);
        return 0;
    }
    const dev_display_lcd_config_t *cfg = raw_cfg;
    const char *sub_type = cfg->sub_type != NULL ? cfg->sub_type : "";

#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_SPI_SUPPORT
    if (strcmp(sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_SPI) == 0) {
        const dev_display_lcd_spi_sub_config_t *spi = &cfg->sub_cfg.spi;
        int host = -1;
        if (bridge_periph_spi_host(spi->spi_name, &host)) {
            bridge_claim_spi_cs(name, owner_tag, host, spi->io_spi_config.cs_gpio_num, &registered);
        } else {
            ESP_LOGD(TAG, "display_lcd %s: unresolved spi peripheral '%s'; skipping cs",
                     name, spi->spi_name != NULL ? spi->spi_name : "?");
        }
        bridge_claim_gpio(name, owner_tag, spi->io_spi_config.dc_gpio_num, &registered);
        bridge_claim_gpio(name, owner_tag, spi->panel_config.reset_gpio_num, &registered);
        return registered;
    }
#endif

    /* DSI/RGB/PARLIO/I80 sub-types are not extracted yet; their shared-pin
     * surface is generally handled by the underlying bus driver. */
    ESP_LOGD(TAG, "display_lcd %s: sub_type '%s' not extracted", name, sub_type);
    return registered;
}
#endif

#ifdef CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
static size_t bridge_extract_lcd_touch(const char *name, const char *owner_tag)
{
    size_t registered = 0;
    void *raw_cfg = NULL;
    if (esp_board_manager_get_device_config(name, &raw_cfg) != ESP_OK || raw_cfg == NULL) {
        ESP_LOGW(TAG, "lcd_touch %s: config unavailable", name);
        return 0;
    }
    const dev_lcd_touch_config_t *cfg = raw_cfg;
    const char *sub_type = cfg->sub_type != NULL ? cfg->sub_type : "";

    bridge_claim_gpio(name, owner_tag, cfg->touch_config.int_gpio_num, &registered);
    bridge_claim_gpio(name, owner_tag, cfg->touch_config.rst_gpio_num, &registered);

#ifdef CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUB_I2C_SUPPORT
    if (strcmp(sub_type, ESP_BOARD_DEVICE_LCD_TOUCH_SUB_TYPE_I2C) == 0) {
        const dev_lcd_touch_i2c_sub_config_t *i2c = &cfg->sub_cfg.i2c;
        int port = -1;
        if (!bridge_periph_i2c_port(i2c->i2c_name, &port)) {
            ESP_LOGD(TAG, "lcd_touch %s: unresolved i2c peripheral '%s'; skipping addr",
                     name, i2c->i2c_name != NULL ? i2c->i2c_name : "?");
            return registered;
        }
        for (size_t i = 0; i < i2c->i2c_addr_count && i < DEV_LCD_TOUCH_I2C_MAX_ADDR_COUNT; ++i) {
            int addr8 = i2c->i2c_addr[i];
            if (!bridge_i2c_addr8_valid(addr8)) {
                continue;
            }
            /* 8-bit board_manager address -> 7-bit registry key. */
            bridge_claim_i2c(name, owner_tag, port, (uint8_t)((addr8 >> 1) & 0x7F), &registered);
        }
        return registered;
    }
#endif

    ESP_LOGD(TAG, "lcd_touch %s: sub_type '%s' not extracted", name, sub_type);
    return registered;
}
#endif

#ifdef CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
static size_t bridge_extract_audio_codec(const char *name, const char *owner_tag)
{
    size_t registered = 0;
    void *raw_cfg = NULL;
    if (esp_board_manager_get_device_config(name, &raw_cfg) != ESP_OK || raw_cfg == NULL) {
        ESP_LOGW(TAG, "audio_codec %s: config unavailable", name);
        return 0;
    }
    const dev_audio_codec_config_t *cfg = raw_cfg;

    /* Direction comes from the I2S peripheral itself: a duplex codec may
     * expose both DAC and ADC, so adc_enabled/dac_enabled alone is not
     * enough. */
#ifdef CONFIG_ESP_BOARD_PERIPH_I2S_SUPPORT
    if (cfg->i2s_cfg.name != NULL && cfg->i2s_cfg.name[0] != '\0') {
        int port = -1;
        bool tx = false, rx = false;
        if (bridge_periph_i2s_port_dir(cfg->i2s_cfg.name, &port, &tx, &rx)) {
            if (tx) {
                bridge_claim_i2s(name, owner_tag, port, true, &registered);
            }
            if (rx) {
                bridge_claim_i2s(name, owner_tag, port, false, &registered);
            }
            if (!tx && !rx) {
                ESP_LOGD(TAG, "audio_codec %s: i2s '%s' has no direction flag",
                         name, cfg->i2s_cfg.name);
            }
        } else {
            ESP_LOGD(TAG, "audio_codec %s: unresolved i2s peripheral '%s'",
                     name, cfg->i2s_cfg.name);
        }
    }
#endif

    if (bridge_i2c_addr8_valid(cfg->i2c_cfg.address)) {
        int port = cfg->i2c_cfg.port;
#ifdef CONFIG_ESP_BOARD_PERIPH_I2C_SUPPORT
        int resolved = -1;
        if (bridge_periph_i2c_port(cfg->i2c_cfg.name, &resolved)) {
            port = resolved;
        }
#endif
        bridge_claim_i2c(name, owner_tag, port,
                         (uint8_t)((cfg->i2c_cfg.address >> 1) & 0x7F), &registered);
    }

    bridge_claim_gpio(name, owner_tag, cfg->pa_cfg.port, &registered);

    return registered;
}
#endif

#ifdef CONFIG_ESP_BOARD_DEV_POWER_CTRL_SUPPORT
static size_t bridge_extract_power_ctrl(const char *name, const char *owner_tag)
{
    size_t registered = 0;
    void *raw_cfg = NULL;
    if (esp_board_manager_get_device_config(name, &raw_cfg) != ESP_OK || raw_cfg == NULL) {
        ESP_LOGW(TAG, "power_ctrl %s: config unavailable", name);
        return 0;
    }
    const dev_power_ctrl_config_t *cfg = raw_cfg;
    const char *sub_type = cfg->sub_type != NULL ? cfg->sub_type : "";

#ifdef CONFIG_ESP_BOARD_PERIPH_GPIO_SUPPORT
    if (strcmp(sub_type, "gpio") == 0) {
        registered += bridge_claim_gpio_periph(name, owner_tag,
                                               cfg->sub_cfg.gpio.gpio_name);
        return registered;
    }
#endif

    ESP_LOGD(TAG, "power_ctrl %s: sub_type '%s' not extracted", name, sub_type);
    return registered;
}
#endif

#ifdef CONFIG_ESP_BOARD_DEV_GPIO_CTRL_SUPPORT
static size_t bridge_extract_gpio_ctrl(const char *name, const char *owner_tag)
{
    void *raw_cfg = NULL;
    if (esp_board_manager_get_device_config(name, &raw_cfg) != ESP_OK || raw_cfg == NULL) {
        ESP_LOGW(TAG, "gpio_ctrl %s: config unavailable", name);
        return 0;
    }
#ifdef CONFIG_ESP_BOARD_PERIPH_GPIO_SUPPORT
    const dev_gpio_ctrl_config_t *cfg = raw_cfg;
    return bridge_claim_gpio_periph(name, owner_tag, cfg->gpio_name);
#else
    (void)owner_tag;
    ESP_LOGD(TAG, "gpio_ctrl %s: gpio peripheral support disabled", name);
    return 0;
#endif
}
#endif

#ifdef CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
static size_t bridge_extract_ledc_ctrl(const char *name, const char *owner_tag)
{
    size_t registered = 0;
    void *raw_cfg = NULL;
    if (esp_board_manager_get_device_config(name, &raw_cfg) != ESP_OK || raw_cfg == NULL) {
        ESP_LOGW(TAG, "ledc_ctrl %s: config unavailable", name);
        return 0;
    }
#ifdef CONFIG_ESP_BOARD_PERIPH_LEDC_SUPPORT
    const dev_ledc_ctrl_config_t *cfg = raw_cfg;
    int pin = -1;
    if (bridge_periph_ledc_gpio(cfg->ledc_name, &pin)) {
        bridge_claim_gpio(name, owner_tag, pin, &registered);
    } else {
        ESP_LOGD(TAG, "ledc_ctrl %s: unresolved ledc peripheral '%s'",
                 name, cfg->ledc_name != NULL ? cfg->ledc_name : "?");
    }
#else
    (void)owner_tag;
    ESP_LOGD(TAG, "ledc_ctrl %s: ledc peripheral support disabled", name);
#endif
    return registered;
}
#endif

#ifdef CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT
static size_t bridge_extract_button(const char *name, const char *owner_tag)
{
    void *raw_cfg = NULL;
    if (esp_board_manager_get_device_config(name, &raw_cfg) != ESP_OK || raw_cfg == NULL) {
        ESP_LOGW(TAG, "button %s: config unavailable", name);
        return 0;
    }
    const dev_button_config_t *cfg = raw_cfg;
    const char *sub_type = cfg->sub_type != NULL ? cfg->sub_type : "";

#ifdef CONFIG_ESP_BOARD_PERIPH_GPIO_SUPPORT
    if (strcmp(sub_type, "gpio") == 0) {
        return bridge_claim_gpio_periph(name, owner_tag,
                                        cfg->sub_cfg.gpio.gpio_name);
    }
#endif

    /* adc_single / adc_multi / custom-driver buttons need family-module
     * arbitration; ADC keys are not part of the current key set. */
    ESP_LOGD(TAG, "button %s: sub_type '%s' not extracted", name, sub_type);
    return 0;
}
#endif

static size_t bridge_extract_custom(const char *name, const char *owner_tag)
{
    (void)owner_tag;
    ESP_LOGW(TAG, "custom device %s: introspection not supported; skipping (family module must lease dev:%s explicitly)",
             name, name);
    return 0;
}

typedef size_t (*bridge_extractor_fn)(const char *name, const char *owner_tag);

typedef struct {
    const char         *type;
    bridge_extractor_fn extract;
} bridge_type_entry_t;

static const bridge_type_entry_t s_known_types[] = {
#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    { ESP_BOARD_DEVICE_TYPE_DISPLAY_LCD, bridge_extract_display_lcd },
#endif
#ifdef CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
    { ESP_BOARD_DEVICE_TYPE_LCD_TOUCH,   bridge_extract_lcd_touch   },
#endif
#ifdef CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
    { ESP_BOARD_DEVICE_TYPE_AUDIO_CODEC, bridge_extract_audio_codec },
#endif
#ifdef CONFIG_ESP_BOARD_DEV_POWER_CTRL_SUPPORT
    { ESP_BOARD_DEVICE_TYPE_POWER_CTRL,  bridge_extract_power_ctrl  },
#endif
#ifdef CONFIG_ESP_BOARD_DEV_GPIO_CTRL_SUPPORT
    { ESP_BOARD_DEVICE_TYPE_GPIO_CTRL,   bridge_extract_gpio_ctrl   },
#endif
#ifdef CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
    { ESP_BOARD_DEVICE_TYPE_LEDC_CTRL,   bridge_extract_ledc_ctrl   },
#endif
#ifdef CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT
    { ESP_BOARD_DEVICE_TYPE_BUTTON,      bridge_extract_button      },
#endif
    { BRIDGE_TYPE_CUSTOM,                bridge_extract_custom      },
};

esp_err_t app_claw_hw_bridge_register_board_devices(void)
{
    /* Idempotent — the bridge holds long-lived leases and must not double-
     * register on a second call. */
    portENTER_CRITICAL(&s_bridge_init_spinlock);
    bool already = s_bridge_registered;
    portEXIT_CRITICAL(&s_bridge_init_spinlock);
    if (already) {
        return ESP_OK;
    }

    esp_err_t err = bridge_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    err = claw_hw_registry_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claw_hw_registry_init failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total_devices_seen = 0;
    size_t total_extractable_devices_seen = 0;
    size_t total_leases_registered = 0;

    for (size_t t = 0; t < sizeof(s_known_types) / sizeof(s_known_types[0]); ++t) {
        const bridge_type_entry_t *entry = &s_known_types[t];
        void *cursor = NULL;
        const char *dev_name = NULL;
        bool is_custom = (strcmp(entry->type, BRIDGE_TYPE_CUSTOM) == 0);
        while (esp_board_device_iterate_name_by_type(entry->type, &cursor, &dev_name) == ESP_OK &&
               dev_name != NULL) {
            char owner_tag[BRIDGE_OWNER_TAG_MAX];
            snprintf(owner_tag, sizeof(owner_tag), "board/%s", dev_name);
            total_devices_seen++;
            if (!is_custom) {
                total_extractable_devices_seen++;
            }
            size_t before = total_leases_registered;
            size_t got = entry->extract(dev_name, owner_tag);
            total_leases_registered += got;
            if (!is_custom && got == 0) {
                ESP_LOGW(TAG, "device %s (type=%s): no underlying resources extracted",
                         dev_name, entry->type);
            }
            (void)before;
        }
    }

    ESP_LOGI(TAG, "board bridge registered %u lease(s) across %u device(s) (%u extractable)",
             (unsigned)total_leases_registered,
             (unsigned)total_devices_seen,
             (unsigned)total_extractable_devices_seen);

    portENTER_CRITICAL(&s_bridge_init_spinlock);
    s_bridge_registered = true;
    portEXIT_CRITICAL(&s_bridge_init_spinlock);

    if (total_extractable_devices_seen > 0 && total_leases_registered == 0) {
        ESP_LOGE(TAG, "board declared %u extractable device(s) but bridge registered nothing",
                 (unsigned)total_extractable_devices_seen);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t app_claw_hw_lease_device(const char *device_name,
                                   const char *owner_tag,
                                   claw_hw_mode_t mode,
                                   claw_hw_lease_handle_t *out_lease,
                                   void **out_device_handle)
{
    if (device_name == NULL || device_name[0] == '\0' ||
        owner_tag == NULL || owner_tag[0] == '\0' || out_lease == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_lease = NULL;
    if (out_device_handle != NULL) {
        *out_device_handle = NULL;
    }

    /* Resolve the device first so an unknown name never creates a stray
     * registry row. */
    void *device_handle = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(device_name, &device_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lease_device: unknown device '%s' (%s)",
                 device_name, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }

    /* Handle callers that run before the bridge init path. */
    err = claw_hw_registry_init();
    if (err != ESP_OK) {
        return err;
    }

    char key[BRIDGE_KEY_BUF];
    if (claw_hw_key_device(key, sizeof(key), device_name) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_hw_claim_config_t claim = {
        .resource      = key,
        .owner_tag     = owner_tag,
        .mode          = mode,
        .on_release    = NULL,
        .user_ctx      = NULL,
        .sub_resources = NULL,
    };
    claw_hw_lease_handle_t lease = NULL;
    err = claw_hw_claim(&claim, &lease);
    if (err != ESP_OK) {
        return err;
    }

    *out_lease = lease;
    if (out_device_handle != NULL) {
        *out_device_handle = device_handle;
    }
    return ESP_OK;
}
