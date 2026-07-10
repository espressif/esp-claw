/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** ESP-Claw framework version number (X.x.x). */
#define CLAW_VERSION_MAJOR 0
/** ESP-Claw framework version number (x.X.x). */
#define CLAW_VERSION_MINOR 1
/** ESP-Claw framework version number (x.x.X). */
#define CLAW_VERSION_PATCH 0

/** Convert an ESP-Claw framework version tuple into an integer for comparisons. */
#define CLAW_VERSION_VAL(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))

/** Current ESP-Claw framework version as an integer. */
#define CLAW_VERSION CLAW_VERSION_VAL(CLAW_VERSION_MAJOR, CLAW_VERSION_MINOR, CLAW_VERSION_PATCH)

/** Return the stable ESP-Claw framework semantic version, such as "0.1.0". */
const char *claw_get_version(void);

/** Return the ESP-Claw framework Git build version from git describe. */
const char *claw_get_git_version(void);

#ifdef __cplusplus
}
#endif
