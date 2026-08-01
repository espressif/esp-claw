/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/*
 * Process-wide accessors for the shared audio compositors started by
 * app_claw_start. Return NULL when the compositor could not be brought up
 * (e.g. on boards without a codec) — callers must handle NULL and fall back
 * to a "codec not available" error path.
 */

#include "audio_capture.h"
#include "audio_mixer.h"

#ifdef __cplusplus
extern "C" {
#endif

audio_mixer_handle_t   app_claw_get_audio_mixer(void);
audio_capture_handle_t app_claw_get_audio_capture(void);

#ifdef __cplusplus
}
#endif
