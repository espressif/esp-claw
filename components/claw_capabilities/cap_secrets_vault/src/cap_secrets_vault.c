/**
 * @file cap_secrets_vault.c
 * @brief secrets_vault Capability Stub for Neon-Claw
 * @author maruf009sultan
 */
#include "cap_secrets_vault.h"
#include "esp_log.h"
static const char *TAG = "cap_secrets_vault";
esp_err_t cap_secrets_vault_init(void) {
    ESP_LOGI(TAG, "secrets_vault initialized (stub)");
    return ESP_OK;
}
esp_err_t cap_secrets_vault_deinit(void) {
    ESP_LOGI(TAG, "secrets_vault deinitialized");
    return ESP_OK;
}
