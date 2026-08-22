set(ROVER_S3_PROJECT_LOG_PREFIX "[rover_s3]")
option(ROVER_S3_STRICT_IDF_PATCH "Fail configure when an ESP-IDF patch cannot be verified or applied" ON)

if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
    message(FATAL_ERROR "${ROVER_S3_PROJECT_LOG_PREFIX} IDF_PATH environment variable is not set")
endif()

function(rover_s3_patch_file_replace FILE_PATH OLD_TEXT NEW_TEXT PATCH_NAME)
    if(NOT EXISTS "${FILE_PATH}")
        message(WARNING "${ROVER_S3_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' skipped: file not found: ${FILE_PATH}")
        return()
    endif()

    file(READ "${FILE_PATH}" FILE_CONTENT)
    string(FIND "${FILE_CONTENT}" "${NEW_TEXT}" FIXED_TEXT_OFFSET)
    if(NOT FIXED_TEXT_OFFSET EQUAL -1)
        message(STATUS "${ROVER_S3_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' already applied")
        return()
    endif()

    string(FIND "${FILE_CONTENT}" "${OLD_TEXT}" OLD_TEXT_OFFSET)
    if(OLD_TEXT_OFFSET EQUAL -1)
        if(ROVER_S3_STRICT_IDF_PATCH)
            message(FATAL_ERROR "${ROVER_S3_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' could not be verified or applied: source pattern not found in ${FILE_PATH}")
        endif()
        message(WARNING "${ROVER_S3_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' skipped: source pattern not found in ${FILE_PATH}")
        return()
    endif()

    string(REPLACE "${OLD_TEXT}" "${NEW_TEXT}" FILE_CONTENT "${FILE_CONTENT}")
    file(WRITE "${FILE_PATH}" "${FILE_CONTENT}")
    message(STATUS "${ROVER_S3_PROJECT_LOG_PREFIX} Applied ESP-IDF patch '${PATCH_NAME}'")
endfunction()

# rover_s3 is pinned to ESP-IDF 5.5.3 (platform espressif32@6.13.0) to avoid a
# PSRAM/MSPI hang present on newer ESP-IDF versions (see sdkconfig.defaults
# comments). components/common/http_reuse's connection-pool code calls
# esp_http_client_set_event_handler(), added upstream after 5.5.3. edge_agent
# carries the same patch (application/edge_agent/tools/cmake/esp_idf_patch.cmake)
# against its own newer, unpinned ESP-IDF checkout for other reasons; rover_s3
# needs it purely for this API gap.
rover_s3_patch_file_replace(
    "$ENV{IDF_PATH}/components/esp_http_client/esp_http_client.c"
    [=[    client->user_data = data;
    return ESP_OK;
}

static esp_err_t _set_config(esp_http_client_handle_t client, const esp_http_client_config_t *config)]=]
    [=[    client->user_data = data;
    return ESP_OK;
}

esp_err_t esp_http_client_set_event_handler(esp_http_client_handle_t client, http_event_handle_cb event_handler)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    client->event_handler = event_handler;
    return ESP_OK;
}

static esp_err_t _set_config(esp_http_client_handle_t client, const esp_http_client_config_t *config)]=]
    "http_client_set_event_handler_impl"
)

rover_s3_patch_file_replace(
    "$ENV{IDF_PATH}/components/esp_http_client/include/esp_http_client.h"
    [=[esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void *data);

/**
 * @brief      Get HTTP client session errno]=]
    [=[esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void *data);

/**
 * @brief      Set the event handler for the client
 *
 * @param[in]  client  The esp_http_client handle
 * @param[in]  event_handler     The event handler
 *
 * @return
 *     - ESP_OK
 *     - ESP_ERR_INVALID_ARG
 */
esp_err_t esp_http_client_set_event_handler(esp_http_client_handle_t client, http_event_handle_cb event_handler);

/**
 * @brief      Get HTTP client session errno]=]
    "http_client_set_event_handler_decl"
)
