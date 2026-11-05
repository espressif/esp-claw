/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_lua_hw.h"

#include <pthread.h>
#include "lauxlib.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char s_registry_key;
void cap_lua_hw_bind(lua_State *L, claw_hw_registry_handle_t registry)
{
    lua_pushlightuserdata(L, registry);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &s_registry_key);
}

claw_hw_registry_handle_t cap_lua_hw_get_registry(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &s_registry_key);
    claw_hw_registry_handle_t registry = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return registry;
}

esp_err_t cap_lua_hw_claim(lua_State *L, cap_lua_hw_lease_t *lease, const char *owner, const char *const *keys, size_t count)
{
    if (!lease || lease->id != CLAW_HW_CLAIM_INVALID) return ESP_ERR_INVALID_STATE;
    lease->registry = cap_lua_hw_get_registry(L);
    if (!lease->registry) return ESP_ERR_INVALID_STATE;
    const claw_hw_claim_config_t config = { .owner = owner, .resources = keys, .resource_count = count };
    return claw_hw_registry_claim(lease->registry, &config, &lease->id);
}

esp_err_t cap_lua_hw_release(cap_lua_hw_lease_t *lease)
{
    if (!lease->id) return ESP_OK;
    esp_err_t err = claw_hw_registry_release(lease->registry, lease->id);
    if (err == ESP_OK) lease->id = CLAW_HW_CLAIM_INVALID;
    return err;
}

esp_err_t cap_lua_hw_claim_gpio(lua_State *L, cap_lua_hw_lease_t *lease, const char *owner, int gpio)
{
    char key[CLAW_HW_KEY_SIZE];
    const char *keys[] = { claw_hw_key_gpio(key, sizeof(key), gpio) };
    if (!keys[0]) return ESP_ERR_INVALID_ARG;
    return cap_lua_hw_claim(L, lease, owner, keys, 1);
}

/* Serialize timer-backed SDK objects with their ESP_TIMER_TASK callbacks. */
static struct {
    pthread_mutex_t lock;
    esp_timer_handle_t timer;
    StaticSemaphore_t done_buffer;
    SemaphoreHandle_t done;
    esp_err_t (*operation)(void *);
    void *arg;
    esp_err_t result;
} s_timer_dispatch = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void cap_lua_hw_timer_dispatch(void *arg)
{
    (void)arg;
    s_timer_dispatch.result = s_timer_dispatch.operation(s_timer_dispatch.arg);
    xSemaphoreGive(s_timer_dispatch.done);
}

esp_err_t cap_lua_hw_timer_call(esp_err_t (*operation)(void *), void *arg)
{
    if (!operation) return ESP_ERR_INVALID_ARG;
    if (pthread_mutex_lock(&s_timer_dispatch.lock) != 0) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    if (!s_timer_dispatch.timer) {
        /* Allocate once before the first SDK object; cleanup needs no allocation. */
        s_timer_dispatch.done = xSemaphoreCreateBinaryStatic(&s_timer_dispatch.done_buffer);
        const esp_timer_create_args_t config = {
            .callback = cap_lua_hw_timer_dispatch, .dispatch_method = ESP_TIMER_TASK, .name = "cap_lua_hw_op",
        };
        err = esp_timer_create(&config, &s_timer_dispatch.timer);
    }
    if (err == ESP_OK) {
        s_timer_dispatch.operation = operation;
        s_timer_dispatch.arg = arg;
        err = esp_timer_start_once(s_timer_dispatch.timer, 1);
        if (err == ESP_OK) {
            xSemaphoreTake(s_timer_dispatch.done, portMAX_DELAY);
            err = s_timer_dispatch.result;
        }
        s_timer_dispatch.arg = NULL;
    }
    pthread_mutex_unlock(&s_timer_dispatch.lock);
    return err;
}
