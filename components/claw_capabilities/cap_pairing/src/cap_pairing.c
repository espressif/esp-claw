/**
 * @file cap_pairing.c
 * @brief Device Pairing Capability Implementation for Neon-Claw
 * 
 * @author maruf009sultan
 * @copyright Neon-Claw Project
 */

#include "cap_pairing.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "string.h"
#include "stdlib.h"
#include "time.h"

static const char *TAG = "cap_pairing";

// Internal storage for pairing requests (simplified for embedded)
// In production, this would use NVS or SPIFFS
#define MAX_PAIRING_REQUESTS 20

static pairing_request_t s_pairing_requests[MAX_PAIRING_REQUESTS];
static bool s_initialized = false;
static int s_request_count = 0;

// Character set for pairing codes (alphanumeric, no ambiguous chars)
static const char s_code_chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
static const int s_code_chars_len = sizeof(s_code_chars) - 1;

/**
 * @brief Generate a random 8-character code
 */
static void generate_random_code(char *out_code) {
    for (int i = 0; i < 8; i++) {
        if (i == 4) {
            out_code[i] = '-';  // Add separator for readability: XXXX-XXXX
        } else {
            int idx = esp_random() % s_code_chars_len;
            out_code[i] = s_code_chars[idx];
        }
    }
    out_code[9] = '\0';
}

/**
 * @brief Get current Unix timestamp
 */
static uint64_t get_current_time(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec;
}

/**
 * @brief Find a request by code
 */
static pairing_request_t* find_request_by_code(const char *code) {
    for (int i = 0; i < s_request_count; i++) {
        if (strcmp(s_pairing_requests[i].code, code) == 0) {
            return &s_pairing_requests[i];
        }
    }
    return NULL;
}

/**
 * @brief Check if channel has reached max pending requests
 */
static bool is_channel_full(pairing_channel_t channel) {
    int count = 0;
    uint64_t now = get_current_time();
    
    for (int i = 0; i < s_request_count; i++) {
        if (s_pairing_requests[i].channel == channel &&
            s_pairing_requests[i].status == PAIRING_STATUS_PENDING &&
            s_pairing_requests[i].expires_at > now) {
            count++;
        }
    }
    
    return count >= PAIRING_MAX_PENDING_PER_CHANNEL;
}

esp_err_t cap_pairing_init(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    memset(s_pairing_requests, 0, sizeof(s_pairing_requests));
    s_request_count = 0;
    s_initialized = true;
    
    ESP_LOGI(TAG, "Pairing capability initialized");
    return ESP_OK;
}

esp_err_t cap_pairing_deinit(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    memset(s_pairing_requests, 0, sizeof(s_pairing_requests));
    s_request_count = 0;
    s_initialized = false;
    
    ESP_LOGI(TAG, "Pairing capability deinitialized");
    return ESP_OK;
}

esp_err_t cap_pairing_generate_code(pairing_channel_t channel, uint32_t ttl_sec, char *out_code) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (channel >= PAIRING_CHANNEL_MAX || out_code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (is_channel_full(channel)) {
        ESP_LOGW(TAG, "Channel %d has max pending requests", channel);
        return ESP_ERR_NO_MEM;
    }
    
    if (s_request_count >= MAX_PAIRING_REQUESTS) {
        ESP_LOGW(TAG, "Max pairing requests reached");
        return ESP_ERR_NO_MEM;
    }
    
    // Generate unique code
    char temp_code[PAIRING_CODE_MAX_LEN];
    int attempts = 0;
    do {
        generate_random_code(temp_code);
        attempts++;
        if (attempts > 10) {
            ESP_LOGE(TAG, "Failed to generate unique code");
            return ESP_FAIL;
        }
    } while (find_request_by_code(temp_code) != NULL);
    
    // Create new request
    pairing_request_t *req = &s_pairing_requests[s_request_count];
    strncpy(req->code, temp_code, PAIRING_CODE_MAX_LEN - 1);
    req->code[PAIRING_CODE_MAX_LEN - 1] = '\0';
    req->channel = channel;
    req->status = PAIRING_STATUS_PENDING;
    req->created_at = get_current_time();
    req->expires_at = req->created_at + (ttl_sec > 0 ? ttl_sec : PAIRING_DEFAULT_TTL_SEC);
    memset(req->user_id, 0, sizeof(req->user_id));
    
    s_request_count++;
    
    strncpy(out_code, temp_code, PAIRING_CODE_MAX_LEN - 1);
    out_code[PAIRING_CODE_MAX_LEN - 1] = '\0';
    
    ESP_LOGI(TAG, "Generated pairing code: %s for channel %d", temp_code, channel);
    return ESP_OK;
}

esp_err_t cap_pairing_accept(const char *code, const char *user_id) {
    if (!s_initialized || code == NULL || user_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    pairing_request_t *req = find_request_by_code(code);
    if (req == NULL) {
        ESP_LOGW(TAG, "Code not found: %s", code);
        return ESP_ERR_NOT_FOUND;
    }
    
    uint64_t now = get_current_time();
    if (req->expires_at <= now) {
        req->status = PAIRING_STATUS_EXPIRED;
        ESP_LOGW(TAG, "Code expired: %s", code);
        return ESP_ERR_TIMEOUT;
    }
    
    if (req->status != PAIRING_STATUS_PENDING) {
        ESP_LOGW(TAG, "Code already processed: %s", code);
        return ESP_ERR_INVALID_STATE;
    }
    
    req->status = PAIRING_STATUS_ACCEPTED;
    strncpy(req->user_id, user_id, sizeof(req->user_id) - 1);
    
    ESP_LOGI(TAG, "Pairing accepted: %s for user %s", code, user_id);
    return ESP_OK;
}

esp_err_t cap_pairing_get_status(const char *code, pairing_status_t *out_status) {
    if (!s_initialized || code == NULL || out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    pairing_request_t *req = find_request_by_code(code);
    if (req == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    
    // Check expiration
    if (req->status == PAIRING_STATUS_PENDING && req->expires_at <= get_current_time()) {
        req->status = PAIRING_STATUS_EXPIRED;
    }
    
    *out_status = req->status;
    return ESP_OK;
}

esp_err_t cap_pairing_get_request(const char *code, pairing_request_t *out_request) {
    if (!s_initialized || code == NULL || out_request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    pairing_request_t *req = find_request_by_code(code);
    if (req == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    
    memcpy(out_request, req, sizeof(pairing_request_t));
    return ESP_OK;
}

esp_err_t cap_pairing_cancel(const char *code) {
    if (!s_initialized || code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    pairing_request_t *req = find_request_by_code(code);
    if (req == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    
    req->status = PAIRING_STATUS_REJECTED;
    ESP_LOGI(TAG, "Pairing cancelled: %s", code);
    return ESP_OK;
}

int cap_pairing_cleanup_expired(void) {
    if (!s_initialized) {
        return 0;
    }
    
    uint64_t now = get_current_time();
    int cleaned = 0;
    
    for (int i = 0; i < s_request_count; i++) {
        if (s_pairing_requests[i].status == PAIRING_STATUS_PENDING &&
            s_pairing_requests[i].expires_at <= now) {
            s_pairing_requests[i].status = PAIRING_STATUS_EXPIRED;
            cleaned++;
        }
    }
    
    if (cleaned > 0) {
        ESP_LOGI(TAG, "Cleaned up %d expired requests", cleaned);
    }
    
    return cleaned;
}

int cap_pairing_get_pending_count(pairing_channel_t channel) {
    if (!s_initialized || channel >= PAIRING_CHANNEL_MAX) {
        return 0;
    }
    
    int count = 0;
    uint64_t now = get_current_time();
    
    for (int i = 0; i < s_request_count; i++) {
        if (s_pairing_requests[i].channel == channel &&
            s_pairing_requests[i].status == PAIRING_STATUS_PENDING &&
            s_pairing_requests[i].expires_at > now) {
            count++;
        }
    }
    
    return count;
}
