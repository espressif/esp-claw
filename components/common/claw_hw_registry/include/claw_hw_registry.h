/*
 * claw_hw_registry — string-keyed hardware lease arbiter.
 * See .agents/spec/hardware-arbiter-spec.md §3.1 / §4.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_HW_MODE_EXCLUSIVE = 0,
    CLAW_HW_MODE_SHARED_READ,
} claw_hw_mode_t;

typedef struct claw_hw_lease_t *claw_hw_lease_handle_t;

/* Invoked from claw_hw_release; must not block or call the registry. */
typedef void (*claw_hw_release_cb_t)(const char *resource,
                                     const char *owner_tag,
                                     void *user_ctx);

typedef struct {
    const char *resource;
    const char *owner_tag;
    claw_hw_mode_t mode;
    claw_hw_release_cb_t on_release;
    void *user_ctx;
    /* Optional NULL-terminated dependent keys claimed atomically under the
     * same owner_tag/mode; any conflict aborts the whole claim. */
    const char *const *sub_resources;
} claw_hw_claim_config_t;

esp_err_t claw_hw_registry_init(void);

esp_err_t claw_hw_claim(const claw_hw_claim_config_t *config,
                        claw_hw_lease_handle_t *out_lease);

esp_err_t claw_hw_release(claw_hw_lease_handle_t lease);

esp_err_t claw_hw_release_by_tag(const char *owner_tag);

/* On success writes the current owner_tag pointer (valid until the next
 * mutation) to *out_tag; ESP_ERR_NOT_FOUND when unclaimed. */
esp_err_t claw_hw_query(const char *resource, const char **out_tag);

/* Callback runs under the registry lock; it must not call any registry API. */
typedef void (*claw_hw_iter_cb_t)(const char *resource,
                                  const char *owner_tag,
                                  claw_hw_mode_t mode,
                                  void *user_ctx);
esp_err_t claw_hw_foreach(claw_hw_iter_cb_t cb, void *user_ctx);

/* Canonical resource-key builders. buflen must be >= 32. */
const char *claw_hw_key_gpio  (char *buf, size_t buflen, int pin);
const char *claw_hw_key_i2c   (char *buf, size_t buflen, int port, uint8_t addr7);
const char *claw_hw_key_spi   (char *buf, size_t buflen, int host, int cs_pin);
const char *claw_hw_key_i2s   (char *buf, size_t buflen, int port, bool tx);
const char *claw_hw_key_rmt   (char *buf, size_t buflen, int channel);
const char *claw_hw_key_adc   (char *buf, size_t buflen, int unit, int channel);
const char *claw_hw_key_device(char *buf, size_t buflen, const char *name);

#ifdef __cplusplus
}
#endif
