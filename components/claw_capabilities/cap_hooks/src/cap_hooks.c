/**
 * @file cap_hooks.c
 * @brief Event Hook System Implementation for Neon-Claw
 * @author maruf009sultan
 */

#include "cap_hooks.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "cap_hooks";

typedef struct {
    int id;
    hook_event_t event;
    hook_priority_t priority;
    hook_handler_t handler;
    void *ctx;
    bool enabled;
} hook_entry_t;

static hook_entry_t s_hooks[HOOK_MAX_HANDLERS];
static bool s_initialized = false;
static int s_next_id = 1;

esp_err_t cap_hooks_init(void) {
    if (s_initialized) return ESP_OK;
    memset(s_hooks, 0, sizeof(s_hooks));
    s_next_id = 1;
    s_initialized = true;
    ESP_LOGI(TAG, "Hook system initialized");
    return ESP_OK;
}

esp_err_t cap_hooks_deinit(void) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    memset(s_hooks, 0, sizeof(s_hooks));
    s_initialized = false;
    ESP_LOGI(TAG, "Hook system deinitialized");
    return ESP_OK;
}

int cap_hooks_register(hook_event_t event, hook_priority_t priority, hook_handler_t handler, void *ctx) {
    if (!s_initialized || handler == NULL || event >= HOOK_EVENT_COUNT) return -1;
    
    for (int i = 0; i < HOOK_MAX_HANDLERS; i++) {
        if (s_hooks[i].id == 0) {
            s_hooks[i].id = s_next_id++;
            s_hooks[i].event = event;
            s_hooks[i].priority = priority;
            s_hooks[i].handler = handler;
            s_hooks[i].ctx = ctx;
            s_hooks[i].enabled = true;
            ESP_LOGD(TAG, "Registered hook %d for event %d", s_hooks[i].id, event);
            return s_hooks[i].id;
        }
    }
    ESP_LOGW(TAG, "No free hook slots");
    return -1;
}

esp_err_t cap_hooks_unregister(int hook_id) {
    if (!s_initialized || hook_id <= 0) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < HOOK_MAX_HANDLERS; i++) {
        if (s_hooks[i].id == hook_id) {
            s_hooks[i].id = 0;
            ESP_LOGD(TAG, "Unregistered hook %d", hook_id);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t cap_hooks_trigger(hook_event_t event, const char *data) {
    if (!s_initialized || event >= HOOK_EVENT_COUNT) return ESP_ERR_INVALID_ARG;
    
    // Sort by priority (simple bubble sort for small array)
    int indices[HOOK_MAX_HANDLERS];
    int count = 0;
    for (int i = 0; i < HOOK_MAX_HANDLERS; i++) {
        if (s_hooks[i].id > 0 && s_hooks[i].enabled && s_hooks[i].event == event) {
            indices[count++] = i;
        }
    }
    
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (s_hooks[indices[j]].priority > s_hooks[indices[j+1]].priority) {
                int tmp = indices[j];
                indices[j] = indices[j+1];
                indices[j+1] = tmp;
            }
        }
    }
    
    for (int i = 0; i < count; i++) {
        hook_entry_t *h = &s_hooks[indices[i]];
        esp_err_t err = h->handler(h->ctx, data);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Hook %d returned error %d", h->id, err);
            if (h->priority == HOOK_PRIORITY_CRITICAL) {
                return err;  // Abort on critical error
            }
        }
    }
    return ESP_OK;
}
