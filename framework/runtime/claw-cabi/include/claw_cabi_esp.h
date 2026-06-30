/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "claw_cabi.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline claw_capability_result_t claw_cabi_ok(void)
{
    return (claw_capability_result_t) {
        .kind = CLAW_CAPABILITY_OK,
        .message = NULL,
    };
}

static inline claw_capability_result_t claw_cabi_error(claw_capability_error_kind_t kind,
                                                       const char *message)
{
    return (claw_capability_result_t) {
        .kind = kind,
        .message = message,
    };
}

static inline claw_capability_result_t claw_cabi_lifecycle_result_from_esp(esp_err_t err)
{
    if (err == ESP_OK) {
        return claw_cabi_ok();
    }

    if (err == ESP_ERR_INVALID_ARG) {
        return claw_cabi_error(CLAW_CAPABILITY_INVALID_ARGUMENT, esp_err_to_name(err));
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return claw_cabi_error(CLAW_CAPABILITY_INVALID_STATE, esp_err_to_name(err));
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return claw_cabi_error(CLAW_CAPABILITY_NOT_FOUND, esp_err_to_name(err));
    }
    return claw_cabi_error(CLAW_CAPABILITY_FAILED, esp_err_to_name(err));
}

static inline esp_err_t claw_cabi_result_to_esp(claw_capability_result_t result)
{
    switch (result.kind) {
    case CLAW_CAPABILITY_OK:
        return ESP_OK;
    case CLAW_CAPABILITY_INVALID_ARGUMENT:
        return ESP_ERR_INVALID_ARG;
    case CLAW_CAPABILITY_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    case CLAW_CAPABILITY_ALREADY_EXISTS:
        return ESP_ERR_INVALID_STATE;
    case CLAW_CAPABILITY_INVALID_STATE:
        return ESP_ERR_INVALID_STATE;
    case CLAW_CAPABILITY_FAILED:
    default:
        return ESP_FAIL;
    }
}

static inline claw_capability_result_t claw_cabi_tool_result_from_esp(esp_err_t err,
                                                                      const char *output_buffer,
                                                                      size_t output_capacity,
                                                                      size_t *output_length,
                                                                      bool *output_success)
{
    if (!output_length || !output_success) {
        return claw_cabi_error(CLAW_CAPABILITY_INVALID_ARGUMENT, "missing tool output pointer");
    }

    *output_success = (err == ESP_OK);
    if (!output_buffer || output_capacity == 0) {
        *output_length = 0;
    } else {
        *output_length = strnlen(output_buffer, output_capacity);
    }

    return claw_cabi_ok();
}

static inline esp_err_t claw_cabi_register_group_esp(claw_capability_registry_t *registry,
                                                     const claw_capability_group_t *group)
{
    return claw_cabi_result_to_esp(claw_capability_register_group(registry, group));
}

#define CLAW_CABI_ESP_TOOL_CALLBACK(callback_name, impl_name)                         \
    static claw_capability_result_t callback_name(const char *arguments_json,         \
                                                  char *output_buffer,               \
                                                  size_t output_capacity,            \
                                                  size_t *output_length,             \
                                                  bool *output_success,              \
                                                  void *user_context)                \
    {                                                                                \
        (void)user_context;                                                          \
        return claw_cabi_tool_result_from_esp(impl_name(arguments_json,              \
                                                        output_buffer,               \
                                                        output_capacity),            \
                                             output_buffer,                          \
                                             output_capacity,                        \
                                             output_length,                          \
                                             output_success);                        \
    }

#define CLAW_CABI_ESP_LIFECYCLE_CALLBACK(callback_name, impl_name)                    \
    static claw_capability_result_t callback_name(void *user_context)                 \
    {                                                                                \
        (void)user_context;                                                          \
        return claw_cabi_lifecycle_result_from_esp(impl_name());                     \
    }

#define CLAW_CABI_ESP_TOOL_DESCRIPTOR(capability_id, capability_description, schema, callback) \
    {                                                                                         \
        .id = (capability_id),                                                                \
        .description = (capability_description),                                               \
        .role = CLAW_CAPABILITY_ROLE_TOOL,                                                     \
        .role_data.tool = {                                                                    \
            .schema_json = (schema),                                                          \
            .execute = (callback),                                                            \
        },                                                                                    \
    }

#define CLAW_CABI_ESP_SERVICE_DESCRIPTOR(capability_id, capability_description, init_cb, start_cb, stop_cb, deinit_cb) \
    {                                                                                                                \
        .id = (capability_id),                                                                                       \
        .description = (capability_description),                                                                      \
        .role = CLAW_CAPABILITY_ROLE_NONE,                                                                            \
        .lifecycle = {                                                                                                \
            .init = (init_cb),                                                                                        \
            .start = (start_cb),                                                                                      \
            .stop = (stop_cb),                                                                                        \
            .deinit = (deinit_cb),                                                                                    \
        },                                                                                                           \
    }

#ifdef __cplusplus
}
#endif
