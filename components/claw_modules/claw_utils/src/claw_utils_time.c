/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_utils_time.h"

#include <sys/time.h>

int64_t claw_utils_time_now_ms(void)
{
    struct timeval tv = {0};

    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000LL) + (tv.tv_usec / 1000LL);
}
