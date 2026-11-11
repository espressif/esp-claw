/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ir_builtin.h"

/* Air-conditioner tables reverse engineered from local remote captures; protocols/<id>.md documents each one. */

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PAIR_LAYOUT(steps_, mark, zero_space, one_space) \
    {.coding = IR_CODING_PAIR, .symbols = {{mark, zero_space}, {mark, one_space}}, .steps = steps_, .step_count = ARRAY_SIZE(steps_)}
#define CAPTURE_SOURCE "local remote captures"
#define TEMPERATURE_16C 160
#define TEMPERATURE_STEP 10
#define FIELD(field_id, segs, raws, ...) \
    {.id = field_id, .segment_count = ARRAY_SIZE(segs), .value_count = ARRAY_SIZE(raws), .segments = segs, .raw = raws, __VA_ARGS__}
#define LINEAR_FIELD(field_id, seg, ...) {.id = field_id, .segment_count = 1, .segments = &seg, __VA_ARGS__}

/* Capability descriptions shared by the tables. */
#define IR_AC_BIT(value) (1UL << (value))
#define IR_AC_DECI(celsius) ((celsius) * 10)
#define IR_AC_FIELDS (IR_FIELD_BIT(IR_FIELD_POWER) | IR_FIELD_BIT(IR_FIELD_MODE) | IR_FIELD_BIT(IR_FIELD_TEMPERATURE) | IR_FIELD_BIT(IR_FIELD_FAN))

/* Power on and off, the five modes and four fan speeds every builtin AC table encodes, whole degrees in [min, max]. */
#define IR_AC_LIMITS(min_celsius, max_celsius) { \
    [IR_FIELD_POWER] = {.mask = IR_AC_BIT(0) | IR_AC_BIT(1)}, \
    [IR_FIELD_MODE] = {.mask = IR_AC_BIT(IR_MODE_AUTO) | IR_AC_BIT(IR_MODE_COOL) | IR_AC_BIT(IR_MODE_DRY) | IR_AC_BIT(IR_MODE_HEAT) | IR_AC_BIT(IR_MODE_FAN)}, \
    [IR_FIELD_FAN] = {.mask = IR_AC_BIT(IR_FAN_AUTO) | IR_AC_BIT(IR_FAN_LOW) | IR_AC_BIT(IR_FAN_MEDIUM) | IR_AC_BIT(IR_FAN_HIGH)}, \
    [IR_FIELD_TEMPERATURE] = {.min = IR_AC_DECI(min_celsius), .max = IR_AC_DECI(max_celsius), .step = IR_AC_DECI(1)}, \
}

/* Power on, cool, 26 °C, auto fan. */
#define IR_AC_DEFAULTS { \
    .present = IR_AC_FIELDS, \
    .value = {[IR_FIELD_POWER] = 1, [IR_FIELD_MODE] = IR_MODE_COOL, [IR_FIELD_TEMPERATURE] = IR_AC_DECI(26), [IR_FIELD_FAN] = IR_FAN_AUTO}, \
}

#define DUP_NIBBLE(t) ((t) | (t) << 4)

/*
 * Gree: two packets of 35 + 32 bits, LSB first. Payload bytes in transmit order:
 * 0-3 packet A head, 4 connect code (3 bits), 5-8 packet A tail, 9-12 packet B head, 13 connect code, 14-17 packet B tail.
 */
#define GREE_MARK_US 590
#define GREE_HEADER IR_MARK(9010), IR_SPACE(4520)
static const ir_step_t s_gree_steps[] = {
    GREE_HEADER, IR_BITS(35), IR_MARK(GREE_MARK_US), IR_SPACE(20020), IR_BITS(32), IR_MARK(GREE_MARK_US), IR_SPACE(40000),
    GREE_HEADER, IR_BITS(35), IR_MARK(GREE_MARK_US), IR_SPACE(20020), IR_BITS(32), IR_MARK(GREE_MARK_US),
};
static const ir_layout_t s_gree_layout = PAIR_LAYOUT(s_gree_steps, GREE_MARK_US, 600, 1700);
static const ir_bit_width_t s_gree_widths[] = {{4, 3}, {13, 3}};
static const uint8_t s_gree_template[] = {
    0x00, 0x00, 0x20, 0x50, 0x02, 0x10, 0x20, 0x00, 0x00,
    0x00, 0x00, 0x20, 0x70, 0x02, 0x00, 0x00, 0x00, 0x00,
};
static const ir_bit_segment_t s_gree_power[] = {{0, 3, 1}, {9, 3, 1}};
static const ir_bit_segment_t s_gree_mode[] = {{0, 0, 3}, {9, 0, 3}};
static const ir_bit_segment_t s_gree_fan[] = {{0, 4, 2}, {9, 4, 2}, {16, 4, 3}};
static const ir_bit_segment_t s_gree_temperature[] = {{1, 0, 4}, {10, 0, 4}};
static const uint32_t s_gree_powers[] = {0, 3};
static const uint32_t s_gree_modes[] = {0, 9, 18, 36, 27};
/* Main fan in both packets, auxiliary fan in packet B. */
static const uint32_t s_gree_fans[] = {0, IR_RAW_NONE, 21, 63, 95};
static const uint32_t s_gree_temperatures[] = {
    DUP_NIBBLE(0), DUP_NIBBLE(1), DUP_NIBBLE(2), DUP_NIBBLE(3), DUP_NIBBLE(4), DUP_NIBBLE(5), DUP_NIBBLE(6), DUP_NIBBLE(7),
    DUP_NIBBLE(8), DUP_NIBBLE(9), DUP_NIBBLE(10), DUP_NIBBLE(11), DUP_NIBBLE(12), DUP_NIBBLE(13), DUP_NIBBLE(14),
};
static const ir_bitfield_t s_gree_fields[] = {
    FIELD(IR_FIELD_POWER, s_gree_power, s_gree_powers),
    FIELD(IR_FIELD_MODE, s_gree_mode, s_gree_modes),
    FIELD(IR_FIELD_FAN, s_gree_fan, s_gree_fans),
    FIELD(IR_FIELD_TEMPERATURE, s_gree_temperature, s_gree_temperatures, .base = TEMPERATURE_16C, .step = TEMPERATURE_STEP),
};
/* Low nibbles of the head bytes and high nibbles of the first three tail bytes, plus 10. */
static const uint8_t s_gree_units_a[] = {1, 3, 5, 7, 10, 12, 14};
static const uint8_t s_gree_units_b[] = {19, 21, 23, 25, 28, 30, 32};
static const ir_check_t s_gree_checks[] = {
    {.op = IR_CHECK_SUM, .flags = IR_CHECK_NIBBLE, .count = ARRAY_SIZE(s_gree_units_a), .target = 16, .bias = 10, .units = s_gree_units_a},
    {.op = IR_CHECK_SUM, .flags = IR_CHECK_NIBBLE, .count = ARRAY_SIZE(s_gree_units_b), .target = 34, .bias = 10, .units = s_gree_units_b},
};
static const ir_message_t s_gree_frame = {
    .name = "frame", .layout = &s_gree_layout, .length = sizeof(s_gree_template), .widths = s_gree_widths, .width_count = ARRAY_SIZE(s_gree_widths),
    .template_bytes = s_gree_template, .fields = s_gree_fields, .field_count = ARRAY_SIZE(s_gree_fields),
    .checks = s_gree_checks, .check_count = ARRAY_SIZE(s_gree_checks),
};
static const ir_protocol_meta_t s_gree_meta = {
    .category = "air_conditioner", .brand = "Gree", .source = CAPTURE_SOURCE, .maturity = "experimental",
    .defaults = IR_AC_DEFAULTS, .limit_fields = IR_AC_FIELDS, .limits = IR_AC_LIMITS(16, 30),
};

const ir_protocol_t ir_protocol_gree = {
    .id = "gree", .carrier_hz = 38000, .flags = IR_PROTOCOL_DECODE, .messages = &s_gree_frame, .message_count = 1, .meta = &s_gree_meta,
};

/* Haier A6: 14 bytes MSB first, byte sum in the last byte. */
#define HAIER_MARK_US 510
#define HAIER_HEADER_MARK_US 3060
static const ir_step_t s_haier_steps[] = {
    IR_MARK(HAIER_HEADER_MARK_US), IR_SPACE(3110), IR_MARK(HAIER_HEADER_MARK_US), IR_SPACE(4560), IR_BITS(112), IR_MARK(HAIER_MARK_US),
};
static const ir_layout_t s_haier_layout = PAIR_LAYOUT(s_haier_steps, HAIER_MARK_US, 590, 1700);
static const uint8_t s_haier_template[] = {0xA6, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x06, 0};
static const ir_bit_segment_t s_haier_temperature = {1, 4, 4};
static const ir_bit_segment_t s_haier_power = {4, 6, 1};
static const ir_bit_segment_t s_haier_mode[] = {{7, 5, 3}, {4, 7, 1}};
static const ir_bit_segment_t s_haier_fan[] = {{5, 5, 3}};
static const uint32_t s_haier_modes[] = {8, 1, 2, 12, 6};
static const uint32_t s_haier_fans[] = {5, IR_RAW_NONE, 3, 2, 1};
static const ir_bitfield_t s_haier_fields[] = {
    LINEAR_FIELD(IR_FIELD_POWER, s_haier_power),
    FIELD(IR_FIELD_MODE, s_haier_mode, s_haier_modes),
    FIELD(IR_FIELD_FAN, s_haier_fan, s_haier_fans),
    LINEAR_FIELD(IR_FIELD_TEMPERATURE, s_haier_temperature, .base = TEMPERATURE_16C, .step = TEMPERATURE_STEP),
};
static const ir_check_t s_haier_check = {.op = IR_CHECK_SUM, .first = 0, .count = 13, .target = 13};
static const ir_message_t s_haier_frame = {
    .name = "frame", .layout = &s_haier_layout, .msb_first = true, .length = sizeof(s_haier_template),
    .template_bytes = s_haier_template, .fields = s_haier_fields, .field_count = ARRAY_SIZE(s_haier_fields),
    .checks = &s_haier_check, .check_count = 1,
};
/* The captured remote stops at 16 and 30 °C, although the 4-bit field reaches 31 °C. */
static const ir_protocol_meta_t s_haier_meta = {
    .category = "air_conditioner", .brand = "Haier", .source = CAPTURE_SOURCE, .maturity = "experimental",
    .defaults = IR_AC_DEFAULTS, .limit_fields = IR_AC_FIELDS, .limits = IR_AC_LIMITS(16, 30),
};

const ir_protocol_t ir_protocol_haier_a6 = {
    .id = "haier.a6", .carrier_hz = 38000, .flags = IR_PROTOCOL_DECODE, .messages = &s_haier_frame, .message_count = 1, .meta = &s_haier_meta,
};
