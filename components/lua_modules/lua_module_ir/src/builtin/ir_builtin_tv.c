/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ir_builtin.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PAIR_LAYOUT(steps_, zero_mark, zero_space, one_mark, one_space) \
    {.coding = IR_CODING_PAIR, .symbols = {{zero_mark, zero_space}, {one_mark, one_space}}, .steps = steps_, .step_count = ARRAY_SIZE(steps_)}

static const ir_bit_segment_t s_byte0 = {0, 0, 8};
static const ir_bit_segment_t s_byte1 = {1, 0, 8};
static const ir_bit_segment_t s_byte2 = {2, 0, 8};
static const ir_bit_segment_t s_word0[] = {{0, 0, 8}, {1, 0, 8}};

/* NEC: address, ~address, command, ~command, LSB first, 108 ms period. */
static const ir_step_t s_nec_steps[] = {IR_MARK(9000), IR_SPACE(4500), IR_BITS(32), IR_MARK(560), IR_PERIOD(108000)};
static const ir_step_t s_nec_repeat_steps[] = {IR_MARK(9000), IR_SPACE(2250), IR_MARK(560), IR_PERIOD(108000)};
static const ir_layout_t s_nec_layout = PAIR_LAYOUT(s_nec_steps, 560, 560, 560, 1690);
static const ir_layout_t s_nec_repeat_layout = PAIR_LAYOUT(s_nec_repeat_steps, 560, 560, 560, 1690);
static const ir_message_t s_nec_repeat = {.name = "repeat", .layout = &s_nec_repeat_layout};

static const ir_bitfield_t s_nec_fields[] = {
    {.id = IR_FIELD_ADDRESS, .segment_count = 1, .segments = &s_byte0},
    {.id = IR_FIELD_COMMAND, .segment_count = 1, .segments = &s_byte2},
};
static const ir_bitfield_t s_wide_address_fields[] = {
    {.id = IR_FIELD_ADDRESS, .segment_count = 2, .segments = s_word0},
    {.id = IR_FIELD_COMMAND, .segment_count = 1, .segments = &s_byte2},
};
static const ir_check_t s_nec_checks[] = {
    {.op = IR_CHECK_COPY, .flags = IR_CHECK_INVERT, .first = 0, .count = 1, .target = 1},
    {.op = IR_CHECK_COPY, .flags = IR_CHECK_INVERT, .first = 2, .count = 1, .target = 3},
};
static const ir_message_t s_nec_frame = {
    .name = "frame", .layout = &s_nec_layout, .length = 4,
    .fields = s_nec_fields, .field_count = ARRAY_SIZE(s_nec_fields), .checks = s_nec_checks, .check_count = 2,
};
static const ir_message_t s_nec_extended_frame = {
    .name = "frame", .layout = &s_nec_layout, .length = 4,
    .fields = s_wide_address_fields, .field_count = ARRAY_SIZE(s_wide_address_fields), .checks = &s_nec_checks[1], .check_count = 1,
};

const ir_protocol_t ir_protocol_nec = {
    .id = "nec", .carrier_hz = 38000, .flags = IR_PROTOCOL_DECODE, .messages = &s_nec_frame, .message_count = 1, .hold = &s_nec_repeat,
};
const ir_protocol_t ir_protocol_nec_extended = {
    .id = "nec.extended", .carrier_hz = 38000, .flags = IR_PROTOCOL_DECODE, .messages = &s_nec_extended_frame, .message_count = 1, .hold = &s_nec_repeat,
};

/* Samsung32: 16-bit address, command, ~command, LSB first. */
static const ir_step_t s_samsung_steps[] = {IR_MARK(4500), IR_SPACE(4500), IR_BITS(32), IR_MARK(560), IR_SPACE(40000)};
static const ir_layout_t s_samsung_layout = PAIR_LAYOUT(s_samsung_steps, 560, 560, 560, 1690);
static const ir_message_t s_samsung_frame = {
    .name = "frame", .layout = &s_samsung_layout, .length = 4,
    .fields = s_wide_address_fields, .field_count = ARRAY_SIZE(s_wide_address_fields), .checks = &s_nec_checks[1], .check_count = 1,
};

const ir_protocol_t ir_protocol_samsung32 = {
    .id = "samsung32", .carrier_hz = 38000, .flags = IR_PROTOCOL_DECODE, .messages = &s_samsung_frame, .message_count = 1,
};

/* JVC: address, command, LSB first; durations in carrier cycles per the SB-Projects description, resent every 50-60 ms. */
#define JVC_CARRIER_HZ 38000
#define JVC_CYCLES(n) ((n) * 1000000 / JVC_CARRIER_HZ)
#define JVC_UNIT_US JVC_CYCLES(20)
#define JVC_PERIOD_US 55000
static const ir_step_t s_jvc_steps[] = {IR_MARK(JVC_CYCLES(320)), IR_SPACE(JVC_CYCLES(160)), IR_BITS(16), IR_MARK(JVC_UNIT_US), IR_PERIOD(JVC_PERIOD_US)};
static const ir_layout_t s_jvc_layout = PAIR_LAYOUT(s_jvc_steps, JVC_UNIT_US, JVC_UNIT_US, JVC_UNIT_US, JVC_CYCLES(60));
static const ir_bitfield_t s_jvc_fields[] = {
    {.id = IR_FIELD_ADDRESS, .segment_count = 1, .segments = &s_byte0},
    {.id = IR_FIELD_COMMAND, .segment_count = 1, .segments = &s_byte1},
};
static const ir_message_t s_jvc_frame = {.name = "frame", .layout = &s_jvc_layout, .length = 2, .fields = s_jvc_fields, .field_count = 2};

const ir_protocol_t ir_protocol_jvc = {
    .id = "jvc", .carrier_hz = JVC_CARRIER_HZ, .flags = IR_PROTOCOL_DECODE | IR_PROTOCOL_UNCHECKED, .messages = &s_jvc_frame, .message_count = 1,
};

/* Sony SIRC: 7-bit command then address, LSB first, 45 ms period, sent 3 times. */
#define SONY_CARRIER_HZ 40000
#define SONY_COPIES 3
#define SONY_STEPS(bits) {IR_MARK(2400), IR_SPACE(600), IR_BITS(bits), IR_PERIOD(45000)}
#define SONY_PROTOCOL(tag, id_, address_segments, last_byte, last_bits) \
    static const ir_step_t s_##tag##_steps[] = SONY_STEPS(8 * (last_byte) + (last_bits)); \
    static const ir_layout_t s_##tag##_layout = PAIR_LAYOUT(s_##tag##_steps, 600, 600, 1200, 600); \
    static const ir_bit_width_t s_##tag##_width = {last_byte, last_bits}; \
    static const ir_bitfield_t s_##tag##_fields[] = { \
        {.id = IR_FIELD_ADDRESS, .segment_count = ARRAY_SIZE(address_segments), .segments = address_segments}, \
        {.id = IR_FIELD_COMMAND, .segment_count = 1, .segments = &s_sony_command}, \
    }; \
    static const ir_message_t s_##tag##_frame = { \
        .name = "frame", .layout = &s_##tag##_layout, .copies = SONY_COPIES, .length = (last_byte) + 1, \
        .widths = &s_##tag##_width, .width_count = 1, .fields = s_##tag##_fields, .field_count = 2, \
    }; \
    const ir_protocol_t ir_protocol_##tag = { \
        .id = id_, .carrier_hz = SONY_CARRIER_HZ, .flags = IR_PROTOCOL_DECODE | IR_PROTOCOL_UNCHECKED, \
        .messages = &s_##tag##_frame, .message_count = 1, \
    }

static const ir_bit_segment_t s_sony_command = {0, 0, 7};
static const ir_bit_segment_t s_sony12_address[] = {{0, 7, 1}, {1, 0, 4}};
static const ir_bit_segment_t s_sony15_address[] = {{0, 7, 1}, {1, 0, 7}};
static const ir_bit_segment_t s_sony20_address[] = {{0, 7, 1}, {1, 0, 8}, {2, 0, 4}};

SONY_PROTOCOL(sony12, "sony12", s_sony12_address, 1, 4);
SONY_PROTOCOL(sony15, "sony15", s_sony15_address, 1, 7);
SONY_PROTOCOL(sony20, "sony20", s_sony20_address, 2, 4);

/* RC5: S1 is the explicit mark, then S2 T A4..A0 C5..C0 MSB first; a one is space then mark. */
static const ir_step_t s_rc5_steps[] = {IR_MARK(889), IR_BITS(13), IR_PERIOD(114000)};
static const ir_layout_t s_rc5_layout = {
    .coding = IR_CODING_BIPHASE, .one_level = 0, .half_us = 889, .steps = s_rc5_steps, .step_count = ARRAY_SIZE(s_rc5_steps),
};
static const ir_bit_width_t s_rc5_width = {0, 5};
static const uint8_t s_rc5_template[] = {0x10, 0x00};
static const ir_bit_segment_t s_rc5_toggle = {0, 3, 1};
static const ir_bit_segment_t s_rc5_address[] = {{1, 6, 2}, {0, 0, 3}};
static const ir_bit_segment_t s_rc5_command = {1, 0, 6};
static const ir_bitfield_t s_rc5_fields[] = {
    {.id = IR_FIELD_ADDRESS, .segment_count = 2, .segments = s_rc5_address},
    {.id = IR_FIELD_COMMAND, .segment_count = 1, .segments = &s_rc5_command},
    {.id = IR_FIELD_TOGGLE, .segment_count = 1, .segments = &s_rc5_toggle},
};
static const ir_message_t s_rc5_frame = {
    .name = "frame", .layout = &s_rc5_layout, .msb_first = true, .length = 2, .widths = &s_rc5_width, .width_count = 1,
    .template_bytes = s_rc5_template, .fields = s_rc5_fields, .field_count = ARRAY_SIZE(s_rc5_fields),
};

const ir_protocol_t ir_protocol_rc5 = {
    .id = "rc5", .carrier_hz = 36000, .flags = IR_PROTOCOL_DECODE | IR_PROTOCOL_UNCHECKED, .messages = &s_rc5_frame, .message_count = 1,
};
