/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "llm/claw_llm_types.h"

esp_err_t claw_media_prepare_asset(const claw_media_asset_t *asset,
                                   const claw_llm_model_profile_t *profile,
                                   size_t image_max_bytes,
                                   claw_media_prepared_t *out_prepared,
                                   char **out_error_message);
void claw_media_prepared_free(claw_media_prepared_t *prepared);
