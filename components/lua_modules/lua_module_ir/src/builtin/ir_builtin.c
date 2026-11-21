/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ir_builtin.h"

#include <string.h>

extern const ir_protocol_t ir_protocol_nec;
extern const ir_protocol_t ir_protocol_nec_extended;
extern const ir_protocol_t ir_protocol_samsung32;
extern const ir_protocol_t ir_protocol_sony12;
extern const ir_protocol_t ir_protocol_sony15;
extern const ir_protocol_t ir_protocol_sony20;
extern const ir_protocol_t ir_protocol_rc5;
extern const ir_protocol_t ir_protocol_jvc;
extern const ir_protocol_t ir_protocol_gree;
extern const ir_protocol_t ir_protocol_haier_a6;
extern const ir_protocol_t ir_protocol_daikin_280;
extern const ir_protocol_t ir_protocol_midea_coolix;
extern const ir_protocol_t ir_protocol_hisense_kelon168;
extern const ir_protocol_t ir_protocol_aux_electra;
extern const ir_protocol_t ir_protocol_tcl_112;
extern const ir_protocol_t ir_protocol_xiaomi_96;

static const ir_protocol_t *const s_protocols[] = {
    &ir_protocol_nec, &ir_protocol_nec_extended, &ir_protocol_samsung32, &ir_protocol_sony12, &ir_protocol_sony15, &ir_protocol_sony20,
    &ir_protocol_rc5, &ir_protocol_jvc, &ir_protocol_gree, &ir_protocol_haier_a6,
    &ir_protocol_daikin_280, &ir_protocol_midea_coolix, &ir_protocol_hisense_kelon168, &ir_protocol_aux_electra, &ir_protocol_tcl_112,
    &ir_protocol_xiaomi_96,
};

const ir_protocol_t *const *ir_builtin_protocols(size_t *count)
{
    if (count) *count = sizeof(s_protocols) / sizeof(s_protocols[0]);
    return s_protocols;
}

const ir_protocol_t *ir_builtin_find(const char *id)
{
    for (size_t i = 0; id && i < sizeof(s_protocols) / sizeof(s_protocols[0]); ++i) {
        if (!strcmp(s_protocols[i]->id, id)) return s_protocols[i];
    }
    return NULL;
}
