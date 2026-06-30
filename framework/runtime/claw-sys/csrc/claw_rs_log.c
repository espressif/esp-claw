/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * C shim bridging the Rust `log` facade to ESP-IDF's logging. ESP_LOGx are
 * macros (no linkable symbol), so the Rust backend (claw_sys::log_backend)
 * calls one thin function per level here, and each forwards to the matching
 * ESP_LOGx macro. Going through the macros (rather than esp_log_write directly)
 * gives the standard "<L> (<ts>) <tag>: <msg>" formatting, color, and the
 * runtime level filtering callers expect.
 *
 * The compile-time level ceiling follows the project-wide `LOG_LOCAL_LEVEL`
 * (i.e. `CONFIG_LOG_MAXIMUM_LEVEL`), exactly like every other component: levels
 * above it compile to no-ops here, and the rest are gated at runtime by
 * `esp_log_level_set` / `CONFIG_LOG_DEFAULT_LEVEL`. To enable more verbose Rust
 * logs, raise that Kconfig value (it applies to C and Rust alike) rather than
 * overriding it for this one shim.
 *
 * Each message is passed as a `"%s"` argument (never as the format string), so
 * a `%` in a Rust log line is treated as data, not a format specifier.
 */
#include "esp_log.h"

void claw_rs_log_error(const char *tag, const char *msg)
{
    ESP_LOGE(tag, "%s", msg);
}

void claw_rs_log_warn(const char *tag, const char *msg)
{
    ESP_LOGW(tag, "%s", msg);
}

void claw_rs_log_info(const char *tag, const char *msg)
{
    ESP_LOGI(tag, "%s", msg);
}

void claw_rs_log_debug(const char *tag, const char *msg)
{
    ESP_LOGD(tag, "%s", msg);
}

void claw_rs_log_verbose(const char *tag, const char *msg)
{
    ESP_LOGV(tag, "%s", msg);
}
