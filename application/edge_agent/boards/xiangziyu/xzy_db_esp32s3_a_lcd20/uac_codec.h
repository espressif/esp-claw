/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "usb/uac_host.h"
#include "dev_audio_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t addr;
    uint8_t iface_num;
    uint32_t preferred_sample_rate;
    bool is_input;
} uac_codec_config_t;

/* Virtual read-only registers used by esp_codec_dev_read_reg() to report negotiated UAC format. */
#define UAC_CODEC_VREG_FORMAT_MAGIC        0x7AC0
#define UAC_CODEC_VREG_FORMAT_SAMPLE_RATE  0x7AC1
#define UAC_CODEC_VREG_FORMAT_CHANNELS     0x7AC2
#define UAC_CODEC_VREG_FORMAT_BITS         0x7AC3
#define UAC_CODEC_VREG_FORMAT_MAGIC_VALUE  0x55414346

dev_audio_codec_handles_t *uac_codec_new_handle(const uac_codec_config_t *config);
void uac_codec_delete_handle(dev_audio_codec_handles_t *codec_handles);

#ifdef __cplusplus
}
#endif
