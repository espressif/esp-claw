/**
 * @file cap_hooks.h
 * @brief Event Hook System for Neon-Claw
 * @author maruf009sultan
 */

#pragma once
#include "esp_err.h"
#include "stdint.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOOK_MAX_HANDLERS 32
#define HOOK_NAME_MAX_LEN 32

typedef enum {
    HOOK_PRE_REQUEST = 0,
    HOOK_POST_REQUEST,
    HOOK_PRE_RESPONSE,
    HOOK_POST_RESPONSE,
    HOOK_ON_ERROR,
    HOOK_ON_CONNECT,
    HOOK_ON_DISCONNECT,
    HOOK_ON_MESSAGE,
    HOOK_EVENT_COUNT
} hook_event_t;

typedef enum {
    HOOK_PRIORITY_CRITICAL = 0,
    HOOK_PRIORITY_HIGH,
    HOOK_PRIORITY_NORMAL,
    HOOK_PRIORITY_LOW,
    HOOK_PRIORITY_BACKGROUND
} hook_priority_t;

typedef esp_err_t (*hook_handler_t)(void *ctx, const char *event_data);

esp_err_t cap_hooks_init(void);
esp_err_t cap_hooks_deinit(void);
int cap_hooks_register(hook_event_t event, hook_priority_t priority, hook_handler_t handler, void *ctx);
esp_err_t cap_hooks_unregister(int hook_id);
esp_err_t cap_hooks_trigger(hook_event_t event, const char *data);

#ifdef __cplusplus
}
#endif
