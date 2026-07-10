#pragma once

#include "esp_err.h"

typedef enum {
    DISPLAY_ARBITER_OWNER_NONE = 0,
    DISPLAY_ARBITER_OWNER_LUA,
    DISPLAY_ARBITER_OWNER_EMOTE,
} display_arbiter_owner_t;

display_arbiter_owner_t display_arbiter_get_owner(void);
esp_err_t display_arbiter_acquire(display_arbiter_owner_t owner);
esp_err_t display_arbiter_release(display_arbiter_owner_t owner);
