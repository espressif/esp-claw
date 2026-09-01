/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "lua_image_convert.h"

esp_err_t lua_image_ppa_transform(const lua_image_source_t *src, lua_image_format_t dst_format, int dst_width, int dst_height,
                                  lua_image_view_t *out);
