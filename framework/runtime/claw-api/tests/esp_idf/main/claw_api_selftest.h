/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * C declaration for the `claw-api-selftest` Rust scenario runner. The call runs
 * a full chat round-trip on the Rust side and returns a small status the Unity
 * test asserts on. Negative values are selftest errors; see the matching Rust
 * constants in claw-api-selftest/src/lib.rs.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Build a ClawApi over EspIdfHttp and issue one chat request to a live
 * OpenAI-compatible endpoint. Returns 0 on a text reply (copied into `out`,
 * NUL-terminated and truncated to fit), or a negative selftest error (error
 * text copied into `out`).
 */
int claw_api_selftest_chat(const char *base_url,
                           const char *api_key,
                           const char *model,
                           const char *user_message,
                           char *out,
                           size_t out_len);

/*
 * Async variant: POST an OpenAI-format chat request via ClawHttpAsync (the
 * async esp_http_client seam), driven by edge-executor on the calling thread.
 * Same return convention as claw_api_selftest_chat.
 */
int claw_api_selftest_chat_async(const char *base_url,
                                 const char *api_key,
                                 const char *model,
                                 const char *user_message,
                                 char *out,
                                 size_t out_len);

#ifdef __cplusplus
}
#endif
