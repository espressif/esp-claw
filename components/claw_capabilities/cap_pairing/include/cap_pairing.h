/**
 * @file cap_pairing.h
 * @brief Device Pairing Capability for Neon-Claw
 * 
 * Implements secure 8-character alphanumeric pairing codes
 * for multi-channel device authentication (Discord, Telegram, WeChat, etc.)
 * 
 * @author maruf009sultan
 * @copyright Neon-Claw Project
 */

#pragma once

#include "esp_err.h"
#include "stdint.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum length of a pairing code string (including null terminator)
 */
#define PAIRING_CODE_MAX_LEN 10

/**
 * @brief Maximum number of pending pairing requests per channel
 */
#define PAIRING_MAX_PENDING_PER_CHANNEL 3

/**
 * @brief Default TTL for pairing codes in seconds (1 hour)
 */
#define PAIRING_DEFAULT_TTL_SEC (3600)

/**
 * @brief Supported pairing channels
 */
typedef enum {
    PAIRING_CHANNEL_DISCORD = 0,
    PAIRING_CHANNEL_TELEGRAM,
    PAIRING_CHANNEL_WECHAT,
    PAIRING_CHANNEL_QQ,
    PAIRING_CHANNEL_FEISHU,
    PAIRING_CHANNEL_MAX
} pairing_channel_t;

/**
 * @brief Pairing request status
 */
typedef enum {
    PAIRING_STATUS_PENDING = 0,
    PAIRING_STATUS_ACCEPTED,
    PAIRING_STATUS_EXPIRED,
    PAIRING_STATUS_REJECTED
} pairing_status_t;

/**
 * @brief Pairing request structure
 */
typedef struct {
    char code[PAIRING_CODE_MAX_LEN];           ///< 8-character alphanumeric code
    pairing_channel_t channel;                 ///< Channel type
    pairing_status_t status;                   ///< Current status
    uint64_t created_at;                       ///< Creation timestamp (Unix epoch)
    uint64_t expires_at;                       ///< Expiration timestamp
    char user_id[32];                          ///< Associated user ID (if accepted)
} pairing_request_t;

/**
 * @brief Initialize the pairing capability
 * 
 * @return ESP_OK on success, ESP_ERR_* otherwise
 */
esp_err_t cap_pairing_init(void);

/**
 * @brief Deinitialize the pairing capability
 * 
 * @return ESP_OK on success, ESP_ERR_* otherwise
 */
esp_err_t cap_pairing_deinit(void);

/**
 * @brief Generate a new pairing code
 * 
 * @param channel The channel to generate the code for
 * @param ttl_sec Time-to-live in seconds (0 for default)
 * @param out_code Buffer to store the generated code (must be at least PAIRING_CODE_MAX_LEN)
 * @return ESP_OK on success, ESP_ERR_* otherwise
 */
esp_err_t cap_pairing_generate_code(pairing_channel_t channel, uint32_t ttl_sec, char *out_code);

/**
 * @brief Accept a pairing code
 * 
 * @param code The pairing code to accept
 * @param user_id The user ID to associate with this pairing
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if code invalid/expired
 */
esp_err_t cap_pairing_accept(const char *code, const char *user_id);

/**
 * @brief Get the status of a pairing code
 * 
 * @param code The pairing code to check
 * @param out_status Pointer to store the status
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if code not found
 */
esp_err_t cap_pairing_get_status(const char *code, pairing_status_t *out_status);

/**
 * @brief Get pairing request details
 * 
 * @param code The pairing code
 * @param out_request Pointer to store the request details
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if code not found
 */
esp_err_t cap_pairing_get_request(const char *code, pairing_request_t *out_request);

/**
 * @brief Cancel a pending pairing request
 * 
 * @param code The pairing code to cancel
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if code not found
 */
esp_err_t cap_pairing_cancel(const char *code);

/**
 * @brief Clean up expired pairing requests
 * 
 * @return Number of cleaned up requests
 */
int cap_pairing_cleanup_expired(void);

/**
 * @brief Get the number of pending requests for a channel
 * 
 * @param channel The channel to check
 * @return Number of pending requests
 */
int cap_pairing_get_pending_count(pairing_channel_t channel);

#ifdef __cplusplus
}
#endif
