/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * C declarations for the `claw-sys-selftest` Rust scenario runners. Each call
 * runs a complete scenario on the Rust side and returns a small status the
 * Unity tests assert on. Negative values are selftest errors; see the matching
 * Rust constants in claw-sys-selftest/src/lib.rs.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drive every log level through the Rust log sink -> ESP_LOGx bridge. Returns 0. */
int claw_sys_selftest_log(void);

/* Spawn + join an EspIdfThread worker and verify its side effect. Returns 0 on success. */
int claw_sys_selftest_thread(void);

/*
 * Blocking POST via the synchronous ClawHttp seam. Returns the HTTP status code
 * (e.g. 200) on a completed request, or a negative selftest error. The response
 * body (or error text) is copied into `out` (NUL-terminated, truncated to fit).
 */
int claw_sys_selftest_sync_http_post(const char *url, char *out, size_t out_len);

/*
 * Run three concurrent POSTs via the async ClawHttpAsync seam, driven by
 * edge-executor. Returns the number of HTTP-200 responses (expect 3), or a
 * negative selftest error. `url` must be HTTPS (async mode is HTTPS-only).
 */
int claw_sys_selftest_run_three_async_http_posts(const char *url);

/* ---------- Resource profiling ---------- */

/* Print baseline heap snapshot (no HTTP). Always returns 0. */
int claw_sys_selftest_resource_baseline(void);

/* Profile a single synchronous HTTP POST. Returns HTTP status or negative error. */
int claw_sys_selftest_resource_sync_http(const char *url);

/*
 * Profile async HTTP at concurrency 1, 2, 3. Prints per-connection overhead,
 * peak usage, and leak check. `url` must be HTTPS. Returns 0 on success.
 */
int claw_sys_selftest_resource_async_http(const char *url);

/*
 * Print a side-by-side summary comparing sync vs async resource usage, plus
 * the async overhead delta. `http_url` for sync, `https_url` for async.
 * Returns 0.
 */
int claw_sys_selftest_resource_summary(const char *http_url, const char *https_url);

#ifdef __cplusplus
}
#endif
