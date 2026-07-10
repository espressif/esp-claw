/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "claw_version.h"

#define CLAW_STRINGIFY_INNER(x) #x
#define CLAW_STRINGIFY(x) CLAW_STRINGIFY_INNER(x)

#ifndef CLAW_GIT_VERSION
#define CLAW_GIT_VERSION "unknown"
#endif

const char *claw_get_version(void)
{
    return CLAW_STRINGIFY(CLAW_VERSION_MAJOR) "." CLAW_STRINGIFY(CLAW_VERSION_MINOR) "." CLAW_STRINGIFY(CLAW_VERSION_PATCH);
}

const char *claw_get_git_version(void)
{
    return CLAW_GIT_VERSION;
}
