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
#define TEMPERATURE_17C 170
#define TEMPERATURE_STEP 10
#define FIELD(field_id, segs, raws, ...) \
    {.id = field_id, .segment_count = ARRAY_SIZE(segs), .value_count = ARRAY_SIZE(raws), .segments = segs, .raw = raws, __VA_ARGS__}
#define LINEAR_FIELD(field_id, seg, ...) {.id = field_id, .segment_count = 1, .segments = &seg, __VA_ARGS__}

/* Capability descriptions shared by the tables. */
#define IR_AC_BIT(value) (1UL << (value))
#define IR_AC_DECI(celsius) ((celsius) * 10)
#define IR_AC_FIELDS (IR_FIELD_BIT(IR_FIELD_POWER) | IR_FIELD_BIT(IR_FIELD_MODE) | IR_FIELD_BIT(IR_FIELD_TEMPERATURE) | IR_FIELD_BIT(IR_FIELD_FAN))
#define IR_AC_BOOL_MASK (IR_AC_BIT(0) | IR_AC_BIT(1))
#define IR_AC_MODE_MASK (IR_AC_BIT(IR_MODE_AUTO) | IR_AC_BIT(IR_MODE_COOL) | IR_AC_BIT(IR_MODE_DRY) | IR_AC_BIT(IR_MODE_HEAT) | IR_AC_BIT(IR_MODE_FAN))
#define IR_AC_FAN_MASK (IR_AC_BIT(IR_FAN_AUTO) | IR_AC_BIT(IR_FAN_LOW) | IR_AC_BIT(IR_FAN_MEDIUM) | IR_AC_BIT(IR_FAN_HIGH))
#define IR_AC_SWING_POSITION(n) (IR_SWING_AUTO + (n))
/* Mode limit of a field the mode does not send, see ir_limit_unused(). */
#define IR_AC_UNUSED_LIMIT {0}

/* Limit entries for power, the five modes, four fan speeds and whole degrees in [min, max]; tables may append more entries. */
#define IR_AC_LIMIT_ITEMS(min_celsius, max_celsius) \
    [IR_FIELD_POWER] = {.mask = IR_AC_BOOL_MASK}, \
    [IR_FIELD_MODE] = {.mask = IR_AC_MODE_MASK}, \
    [IR_FIELD_FAN] = {.mask = IR_AC_FAN_MASK}, \
    [IR_FIELD_TEMPERATURE] = {.min = IR_AC_DECI(min_celsius), .max = IR_AC_DECI(max_celsius), .step = IR_AC_DECI(1)}

/* Power on, cool, 26 °C, auto fan. */
#define IR_AC_DEFAULT_ITEMS [IR_FIELD_POWER] = 1, [IR_FIELD_MODE] = IR_MODE_COOL, [IR_FIELD_TEMPERATURE] = IR_AC_DECI(26), [IR_FIELD_FAN] = IR_FAN_AUTO
#define IR_AC_DEFAULTS {.present = IR_AC_FIELDS, .value = {IR_AC_DEFAULT_ITEMS}}

#define KEY_MASK(last_key) (IR_AC_BIT((last_key) + 1) - 1)
#define SWING_MASK (IR_AC_BIT(IR_SWING_OFF) | IR_AC_BIT(IR_SWING_AUTO))

static const ir_values_t s_power_on = {.present = IR_FIELD_BIT(IR_FIELD_POWER), .value = {[IR_FIELD_POWER] = 1}};
static const ir_values_t s_power_off = {.present = IR_FIELD_BIT(IR_FIELD_POWER), .value = {[IR_FIELD_POWER] = 0}};
static const ir_values_t s_key_power = {.present = IR_FIELD_BIT(IR_FIELD_KEY), .value = {[IR_FIELD_KEY] = IR_KEY_POWER}};

#define IMPLIED_ON(field_id, field_value) \
    {.present = IR_FIELD_BIT(IR_FIELD_POWER) | IR_FIELD_BIT(field_id), .value = {[IR_FIELD_POWER] = 1, [field_id] = field_value}}

static const ir_values_t s_mode_dry = IMPLIED_ON(IR_FIELD_MODE, IR_MODE_DRY);
static const ir_values_t s_mode_fan = IMPLIED_ON(IR_FIELD_MODE, IR_MODE_FAN);
static const ir_values_t s_mode_auto = IMPLIED_ON(IR_FIELD_MODE, IR_MODE_AUTO);
static const ir_values_t s_mode_heat = IMPLIED_ON(IR_FIELD_MODE, IR_MODE_HEAT);
static const ir_values_t s_key_temp_down = IMPLIED_ON(IR_FIELD_KEY, IR_KEY_TEMP_DOWN);
static const ir_values_t s_key_swing = IMPLIED_ON(IR_FIELD_KEY, IR_KEY_SWING_V);
static const ir_values_t s_key_swing_fix = IMPLIED_ON(IR_FIELD_KEY, IR_KEY_SWING_V_FIX);

static const uint32_t s_swing_off_auto_0_15[] = {0, 15};

/* Mode limits of modes whose frames carry a fixed temperature code instead of the setpoint. */
#define NO_TEMPERATURE IR_FIELD_BIT(IR_FIELD_TEMPERATURE)
static const ir_limit_t s_no_temperature_limits[IR_FIELD_COUNT] = {[IR_FIELD_TEMPERATURE] = IR_AC_UNUSED_LIMIT};

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
/* Swing flag in both packets and the vane position nibble, 1 = sweep. */
static const ir_bit_segment_t s_gree_swing[] = {{0, 6, 1}, {9, 6, 1}, {5, 0, 4}};
static const uint32_t s_gree_powers[] = {0, 3};
static const uint32_t s_gree_swings[] = {0, 7};
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
    FIELD(IR_FIELD_SWING_V, s_gree_swing, s_gree_swings),
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
#define GREE_FIELDS (IR_AC_FIELDS | IR_FIELD_BIT(IR_FIELD_SWING_V))
static const ir_protocol_meta_t s_gree_meta = {
    .category = "air_conditioner", .brand = "Gree", .source = CAPTURE_SOURCE, .maturity = "experimental",
    .defaults = {.present = GREE_FIELDS, .value = {IR_AC_DEFAULT_ITEMS}}, .limit_fields = GREE_FIELDS,
    .limits = {IR_AC_LIMIT_ITEMS(16, 30), [IR_FIELD_SWING_V] = {.mask = SWING_MASK}},
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
static const ir_bit_segment_t s_haier_swing[] = {{1, 1, 3}};
static const ir_bit_segment_t s_haier_power = {4, 6, 1};
static const ir_bit_segment_t s_haier_mode[] = {{7, 5, 3}, {4, 7, 1}};
static const ir_bit_segment_t s_haier_fan[] = {{5, 5, 3}};
static const ir_bit_segment_t s_haier_sleep[] = {{5, 3, 1}, {8, 7, 1}};
static const ir_bit_segment_t s_haier_key[] = {{12, 0, 3}};
static const uint32_t s_haier_modes[] = {8, 1, 2, 12, 6};
static const uint32_t s_haier_fans[] = {5, IR_RAW_NONE, 3, 2, 1};
/* Sweep, then fixed vanes from top to bottom. */
static const uint32_t s_haier_swings[] = {IR_RAW_NONE, 6, 5, 4, 3, 2, 1, 0};
static const uint32_t s_haier_sleeps[] = {0, 3};
/* Action codes; the remote sends swing_v for fixed vanes too and power for sleep. */
static const uint32_t s_haier_keys[] = {5, 6, 0, 1, 4, 2};
static const ir_bitfield_t s_haier_fields[] = {
    LINEAR_FIELD(IR_FIELD_POWER, s_haier_power),
    FIELD(IR_FIELD_MODE, s_haier_mode, s_haier_modes),
    FIELD(IR_FIELD_FAN, s_haier_fan, s_haier_fans),
    LINEAR_FIELD(IR_FIELD_TEMPERATURE, s_haier_temperature, .base = TEMPERATURE_16C, .step = TEMPERATURE_STEP),
    FIELD(IR_FIELD_SWING_V, s_haier_swing, s_haier_swings),
    FIELD(IR_FIELD_SLEEP, s_haier_sleep, s_haier_sleeps),
    FIELD(IR_FIELD_KEY, s_haier_key, s_haier_keys),
};
static const ir_check_t s_haier_check = {.op = IR_CHECK_SUM, .first = 0, .count = 13, .target = 13};
static const ir_message_t s_haier_frame = {
    .name = "frame", .layout = &s_haier_layout, .msb_first = true, .length = sizeof(s_haier_template),
    .template_bytes = s_haier_template, .fields = s_haier_fields, .field_count = ARRAY_SIZE(s_haier_fields),
    .checks = &s_haier_check, .check_count = 1,
};
#define HAIER_SWING_MIDDLE IR_AC_SWING_POSITION(5)
#define HAIER_SWING_BOTTOM IR_AC_SWING_POSITION(6)
#define HAIER_FIELDS (IR_AC_FIELDS | IR_FIELD_BIT(IR_FIELD_SWING_V) | IR_FIELD_BIT(IR_FIELD_SLEEP) | IR_FIELD_BIT(IR_FIELD_KEY))
/* The captured remote stops at 16 and 30 °C, although the 4-bit field reaches 31 °C. */
static const ir_protocol_meta_t s_haier_meta = {
    .category = "air_conditioner", .brand = "Haier", .source = CAPTURE_SOURCE, .maturity = "experimental",
    .defaults = {.present = HAIER_FIELDS & ~IR_FIELD_BIT(IR_FIELD_KEY), .value = {IR_AC_DEFAULT_ITEMS, [IR_FIELD_SWING_V] = HAIER_SWING_MIDDLE}},
    .limit_fields = HAIER_FIELDS,
    .limits = {
        IR_AC_LIMIT_ITEMS(16, 30),
        [IR_FIELD_SWING_V] = {.mask = (IR_AC_BIT(HAIER_SWING_BOTTOM + 1) - 1) & ~IR_AC_BIT(IR_SWING_OFF)},
        [IR_FIELD_SLEEP] = {.mask = IR_AC_BOOL_MASK},
        [IR_FIELD_KEY] = {.mask = IR_AC_BIT(ARRAY_SIZE(s_haier_keys)) - 1},
    },
};

const ir_protocol_t ir_protocol_haier_a6 = {
    .id = "haier.a6", .carrier_hz = 38000, .flags = IR_PROTOCOL_DECODE, .messages = &s_haier_frame, .message_count = 1, .meta = &s_haier_meta,
};

/*
 * Daikin 280: five zero bits, then LSB-first sections of 8, 8 and 19 bytes. Sections 1 and 2 are constant,
 * section 3 carries the state and ends with its byte sum.
 */
static const ir_step_t s_daikin_steps[] = {
    IR_BITS(5), IR_MARK(400), IR_SPACE(25200),
    IR_MARK(3450), IR_SPACE(1730), IR_BITS(64), IR_MARK(400), IR_SPACE(34650),
    IR_MARK(3450), IR_SPACE(1730), IR_BITS(64), IR_MARK(400), IR_SPACE(34650),
    IR_MARK(3450), IR_SPACE(1730), IR_BITS(152), IR_MARK(400),
};
static const ir_layout_t s_daikin_layout = PAIR_LAYOUT(s_daikin_steps, 400, 450, 1300);
static const ir_bit_width_t s_daikin_width = {0, 5};
#define DAIKIN_HEAD 0x00, 0x11, 0xDA, 0x27, 0x00, 0xC5, 0x00, 0x00, 0xD7, 0x11, 0xDA, 0x27, 0x00, 0x42, 0x00, 0x00, 0x54, 0x11, 0xDA, 0x27, 0x00, 0x00
#define DAIKIN_TAIL 0x00, 0x06, 0x60, 0x00, 0x00, 0xC1, 0x80, 0x00, 0x00
/* Byte 22: mode << 4 | 0x08 | power, byte 23: degrees << 1, byte 25: fan << 4 | vertical swing, byte 26: horizontal swing. */
static const uint8_t s_daikin_template[] = {DAIKIN_HEAD, 0x39, 0x34, 0x00, 0xA0, 0x0F, DAIKIN_TAIL};
static const uint8_t s_daikin_dry_template[] = {DAIKIN_HEAD, 0x29, 0xC0, 0x00, 0xA0, 0x0F, DAIKIN_TAIL};
static const uint8_t s_daikin_fan_template[] = {DAIKIN_HEAD, 0x69, 0x32, 0x00, 0xA0, 0x0F, DAIKIN_TAIL};
static const uint8_t s_daikin_off_template[] = {DAIKIN_HEAD, 0x38, 0x34, 0x00, 0xA0, 0x00, DAIKIN_TAIL};
static const ir_bit_segment_t s_daikin_mode[] = {{22, 4, 3}};
static const ir_bit_segment_t s_daikin_temperature = {23, 1, 6};
static const ir_bit_segment_t s_daikin_fan[] = {{25, 4, 4}};
static const ir_bit_segment_t s_daikin_swing_v[] = {{25, 0, 4}};
static const ir_bit_segment_t s_daikin_swing_h[] = {{26, 0, 4}};
static const uint32_t s_daikin_modes[] = {0, 3, IR_RAW_NONE, 4, IR_RAW_NONE};
static const uint32_t s_daikin_off_modes[] = {0, 3, 2, 4, 6};
static const uint32_t s_daikin_fans[] = {10, IR_RAW_NONE, 3, 5, 7};
#define DAIKIN_FAN_SWING_FIELDS \
    FIELD(IR_FIELD_FAN, s_daikin_fan, s_daikin_fans), \
    FIELD(IR_FIELD_SWING_V, s_daikin_swing_v, s_swing_off_auto_0_15)
static const ir_bitfield_t s_daikin_fields[] = {
    FIELD(IR_FIELD_MODE, s_daikin_mode, s_daikin_modes),
    LINEAR_FIELD(IR_FIELD_TEMPERATURE, s_daikin_temperature, .step = TEMPERATURE_STEP),
    DAIKIN_FAN_SWING_FIELDS,
    FIELD(IR_FIELD_SWING_H, s_daikin_swing_h, s_swing_off_auto_0_15),
};
/* Dry and fan modes carry fixed temperature codes. */
static const ir_bitfield_t s_daikin_fixed_fields[] = {DAIKIN_FAN_SWING_FIELDS, FIELD(IR_FIELD_SWING_H, s_daikin_swing_h, s_swing_off_auto_0_15)};
static const ir_bitfield_t s_daikin_off_fields[] = {
    FIELD(IR_FIELD_MODE, s_daikin_mode, s_daikin_off_modes),
    LINEAR_FIELD(IR_FIELD_TEMPERATURE, s_daikin_temperature, .step = TEMPERATURE_STEP),
    DAIKIN_FAN_SWING_FIELDS,
};
static const ir_check_t s_daikin_check = {.op = IR_CHECK_SUM, .first = 17, .count = 18, .target = 35};
#define DAIKIN_MESSAGE(name_, template_, fields_, implied_) { \
    .name = name_, .layout = &s_daikin_layout, .length = sizeof(template_), .widths = &s_daikin_width, .width_count = 1, \
    .template_bytes = template_, .fields = fields_, .field_count = ARRAY_SIZE(fields_), .checks = &s_daikin_check, .check_count = 1, \
    .implied = implied_, \
}
/* Dry and fan come first: the generic frame would read their fixed temperature codes as degrees. */
static const ir_message_t s_daikin_messages[] = {
    DAIKIN_MESSAGE("dry", s_daikin_dry_template, s_daikin_fixed_fields, &s_mode_dry),
    DAIKIN_MESSAGE("fan", s_daikin_fan_template, s_daikin_fixed_fields, &s_mode_fan),
    DAIKIN_MESSAGE("frame", s_daikin_template, s_daikin_fields, &s_power_on),
    DAIKIN_MESSAGE("off", s_daikin_off_template, s_daikin_off_fields, &s_power_off),
};
#define DAIKIN_FIELDS (IR_AC_FIELDS | IR_FIELD_BIT(IR_FIELD_SWING_V) | IR_FIELD_BIT(IR_FIELD_SWING_H))
static const ir_limit_t s_daikin_dry_limits[IR_FIELD_COUNT] = {[IR_FIELD_FAN] = {.mask = IR_AC_BIT(IR_FAN_AUTO)}, [IR_FIELD_TEMPERATURE] = IR_AC_UNUSED_LIMIT};
static const ir_protocol_meta_t s_daikin_meta = {
    .category = "air_conditioner", .brand = "Daikin", .model = "280-bit", .source = CAPTURE_SOURCE, .maturity = "experimental",
    .defaults = {.present = DAIKIN_FIELDS, .value = {IR_AC_DEFAULT_ITEMS, [IR_FIELD_SWING_H] = IR_SWING_AUTO}},
    .limit_fields = DAIKIN_FIELDS,
    .limits = {IR_AC_LIMIT_ITEMS(18, 30), [IR_FIELD_SWING_V] = {.mask = SWING_MASK}, [IR_FIELD_SWING_H] = {.mask = SWING_MASK}},
    .mode_limit_fields = {[IR_MODE_DRY] = IR_FIELD_BIT(IR_FIELD_FAN) | NO_TEMPERATURE, [IR_MODE_FAN] = NO_TEMPERATURE},
    .mode_limits = {[IR_MODE_DRY] = s_daikin_dry_limits, [IR_MODE_FAN] = s_no_temperature_limits},
};

const ir_protocol_t ir_protocol_daikin_280 = {
    .id = "daikin.280", .carrier_hz = 38000, .flags = IR_PROTOCOL_DECODE, .messages = s_daikin_messages,
    .message_count = ARRAY_SIZE(s_daikin_messages), .meta = &s_daikin_meta,
};

/*
 * Midea (Coolix family), MSB first: B2 4D fan ~fan temperature|mode ~ sent twice, then a D5 extension packet
 * whose byte depends on the fan, closed by a byte sum. Off and vane commands are fixed packets sent twice.
 */
#define MIDEA_MARK_US 420
#define MIDEA_PACKET IR_MARK(4400), IR_SPACE(4400), IR_BITS(48), IR_MARK(MIDEA_MARK_US)
#define MIDEA_GAP IR_SPACE(5300)
static const ir_step_t s_midea_steps[] = {MIDEA_PACKET, MIDEA_GAP, MIDEA_PACKET, MIDEA_GAP, MIDEA_PACKET};
static const ir_step_t s_midea_command_steps[] = {MIDEA_PACKET, MIDEA_GAP, MIDEA_PACKET};
static const ir_layout_t s_midea_layout = PAIR_LAYOUT(s_midea_steps, MIDEA_MARK_US, 650, 1700);
static const ir_layout_t s_midea_command_layout = PAIR_LAYOUT(s_midea_command_steps, MIDEA_MARK_US, 650, 1700);
#define MIDEA_EXTENSION(code) 0xD5, code, 0x00, 0x00, 0x00, 0x00
#define MIDEA_FAN_AUTO_CODE 0x66
#define MIDEA_FIXED_FAN_CODE 0x65
static const uint8_t s_midea_template[] = {0xB2, 0x4D, 0xBF, 0x40, 0xD0, 0x2F, 0xB2, 0x4D, 0xBF, 0x40, 0xD0, 0x2F, MIDEA_EXTENSION(MIDEA_FAN_AUTO_CODE)};
static const uint8_t s_midea_heat_template[] = {0xB2, 0x4D, 0xBF, 0x40, 0xDC, 0x23, 0xB2, 0x4D, 0xBF, 0x40, 0xDC, 0x2A, MIDEA_EXTENSION(MIDEA_FAN_AUTO_CODE)};
static const uint8_t s_midea_fan_template[] = {0xB2, 0x4D, 0xBF, 0x40, 0xE4, 0x1B, 0xB2, 0x4D, 0xBF, 0x40, 0xE4, 0x1B, MIDEA_EXTENSION(MIDEA_FAN_AUTO_CODE)};
static const uint8_t s_midea_auto_template[] = {0xB2, 0x4D, 0x1F, 0xE0, 0xD8, 0x27, 0xB2, 0x4D, 0x1F, 0xE0, 0xD8, 0x27, MIDEA_EXTENSION(MIDEA_FIXED_FAN_CODE)};
static const uint8_t s_midea_dry_template[] = {0xB2, 0x4D, 0x1F, 0xE0, 0xD4, 0x2B, 0xB2, 0x4D, 0x1F, 0xE0, 0xD4, 0x2B, MIDEA_EXTENSION(MIDEA_FIXED_FAN_CODE)};
static const uint8_t s_midea_off_template[] = {0xB2, 0x4D, 0x7B, 0x84, 0xE0, 0x1F, 0xB2, 0x4D, 0x7B, 0x84, 0xE0, 0x1F};
static const uint8_t s_midea_swing_template[] = {0xB9, 0x46, 0xF5, 0x0A, 0x04, 0xFB, 0xB9, 0x46, 0xF5, 0x0A, 0x04, 0xFB};
static const uint8_t s_midea_swing_fix_template[] = {0xB9, 0x46, 0xF5, 0x0A, 0x05, 0xFA, 0xB9, 0x46, 0xF5, 0x0A, 0x05, 0xFA};
static const ir_bit_segment_t s_midea_mode[] = {{4, 2, 2}};
static const ir_bit_segment_t s_midea_temperature[] = {{4, 4, 4}};
/* Fan code in the state packets, then the extension byte. */
static const ir_bit_segment_t s_midea_fan[] = {{2, 5, 3}, {13, 0, 8}};
static const uint32_t s_midea_modes[] = {IR_RAW_NONE, 0, IR_RAW_NONE, 3, IR_RAW_NONE};
static const uint32_t s_midea_temperatures[] = {0x0, 0x1, 0x3, 0x2, 0x6, 0x7, 0x5, 0x4, 0xC, 0xD, 0x9, 0x8, 0xA, 0xB};
#define MIDEA_FAN(code, extension) ((code) | (extension) << 3)
static const uint32_t s_midea_fans[] = {MIDEA_FAN(5, MIDEA_FAN_AUTO_CODE), IR_RAW_NONE, MIDEA_FAN(4, 0x28), MIDEA_FAN(2, 0x3C), MIDEA_FAN(1, 0x64)};
#define MIDEA_TEMPERATURE_FIELD FIELD(IR_FIELD_TEMPERATURE, s_midea_temperature, s_midea_temperatures, .base = TEMPERATURE_17C, .step = TEMPERATURE_STEP)
#define MIDEA_FAN_FIELD FIELD(IR_FIELD_FAN, s_midea_fan, s_midea_fans)
static const ir_bitfield_t s_midea_fields[] = {FIELD(IR_FIELD_MODE, s_midea_mode, s_midea_modes), MIDEA_TEMPERATURE_FIELD, MIDEA_FAN_FIELD};
static const ir_bitfield_t s_midea_heat_fields[] = {MIDEA_TEMPERATURE_FIELD, MIDEA_FAN_FIELD};
static const ir_bitfield_t s_midea_fan_fields[] = {MIDEA_FAN_FIELD};
static const ir_bitfield_t s_midea_auto_fields[] = {MIDEA_TEMPERATURE_FIELD};
#define MIDEA_INVERSE_CHECKS \
    {.op = IR_CHECK_COPY, .flags = IR_CHECK_INVERT, .first = 2, .count = 1, .target = 3}, \
    {.op = IR_CHECK_COPY, .flags = IR_CHECK_INVERT, .first = 4, .count = 1, .target = 5}
#define MIDEA_EXTENSION_CHECK {.op = IR_CHECK_SUM, .first = 12, .count = 5, .target = 17}
static const ir_check_t s_midea_checks[] = {MIDEA_INVERSE_CHECKS, {.op = IR_CHECK_COPY, .first = 0, .count = 6, .target = 6}, MIDEA_EXTENSION_CHECK};
/* The captured remote flips bits 0 and 3 of the last inverted byte of the second heat packet. */
#define MIDEA_HEAT_FLIP 0x09
static const ir_check_t s_midea_heat_checks[] = {
    MIDEA_INVERSE_CHECKS,
    {.op = IR_CHECK_COPY, .first = 0, .count = 5, .target = 6},
    {.op = IR_CHECK_XOR, .flags = IR_CHECK_INVERT, .first = 4, .count = 1, .target = 11, .bias = MIDEA_HEAT_FLIP},
    MIDEA_EXTENSION_CHECK,
};
#define MIDEA_MESSAGE(name_, template_, fields_, checks_, implied_) { \
    .name = name_, .layout = &s_midea_layout, .msb_first = true, .length = sizeof(template_), .template_bytes = template_, \
    .fields = fields_, .field_count = ARRAY_SIZE(fields_), .checks = checks_, .check_count = ARRAY_SIZE(checks_), .implied = implied_, \
}
#define MIDEA_COMMAND(name_, template_, implied_) { \
    .name = name_, .layout = &s_midea_command_layout, .msb_first = true, .length = sizeof(template_), .template_bytes = template_, \
    .implied = implied_, \
}
/* Heat precedes frame so standard heat packets, rejected by the heat checks, still decode through frame. */
static const ir_message_t s_midea_messages[] = {
    MIDEA_COMMAND("off", s_midea_off_template, &s_power_off),
    MIDEA_COMMAND("swing_v", s_midea_swing_template, &s_key_swing),
    MIDEA_COMMAND("swing_v_fix", s_midea_swing_fix_template, &s_key_swing_fix),
    MIDEA_MESSAGE("heat", s_midea_heat_template, s_midea_heat_fields, s_midea_heat_checks, &s_mode_heat),
    MIDEA_MESSAGE("auto", s_midea_auto_template, s_midea_auto_fields, s_midea_checks, &s_mode_auto),
    MIDEA_MESSAGE("dry", s_midea_dry_template, s_midea_auto_fields, s_midea_checks, &s_mode_dry),
    MIDEA_MESSAGE("fan", s_midea_fan_template, s_midea_fan_fields, s_midea_checks, &s_mode_fan),
    MIDEA_MESSAGE("frame", s_midea_template, s_midea_fields, s_midea_checks, &s_power_on),
};
#define AC_KEY_FIELDS (IR_AC_FIELDS | IR_FIELD_BIT(IR_FIELD_KEY))
static const ir_limit_t s_midea_fixed_fan_limits[IR_FIELD_COUNT] = {[IR_FIELD_FAN] = {.mask = IR_AC_BIT(IR_FAN_AUTO)}};
static const ir_protocol_meta_t s_midea_meta = {
    .category = "air_conditioner", .brand = "Midea", .model = "Coolix family", .source = CAPTURE_SOURCE, .maturity = "experimental",
    .defaults = IR_AC_DEFAULTS, .limit_fields = AC_KEY_FIELDS,
    .limits = {IR_AC_LIMIT_ITEMS(17, 30), [IR_FIELD_KEY] = {.mask = KEY_MASK(IR_KEY_SWING_V_FIX)}},
    .mode_limit_fields = {[IR_MODE_AUTO] = IR_FIELD_BIT(IR_FIELD_FAN), [IR_MODE_DRY] = IR_FIELD_BIT(IR_FIELD_FAN), [IR_MODE_FAN] = NO_TEMPERATURE},
    .mode_limits = {[IR_MODE_AUTO] = s_midea_fixed_fan_limits, [IR_MODE_DRY] = s_midea_fixed_fan_limits, [IR_MODE_FAN] = s_no_temperature_limits},
};

const ir_protocol_t ir_protocol_midea_coolix = {
    .id = "midea.coolix", .carrier_hz = 38000, .flags = IR_PROTOCOL_DECODE, .messages = s_midea_messages,
    .message_count = ARRAY_SIZE(s_midea_messages), .meta = &s_midea_meta,
};

/*
 * Hisense (Kelon168 family), LSB first: sections of 6, 8 and 7 bytes. The second section ends with the XOR of
 * bytes 2-12, the third with the XOR of its own bytes. Byte 15 is the key code; power is a toggle.
 */
static const ir_step_t s_hisense_steps[] = {
    IR_MARK(9000), IR_SPACE(4500), IR_BITS(48), IR_MARK(460), IR_SPACE(8100), IR_BITS(64), IR_MARK(460), IR_SPACE(8100), IR_BITS(56), IR_MARK(460),
};
static const ir_layout_t s_hisense_layout = PAIR_LAYOUT(s_hisense_steps, 460, 700, 1750);
#define HISENSE_KEY_POWER 0x01
#define HISENSE_KEY_TEMPERATURE 0x02
#define HISENSE_KEY_SWING 0x07
static const uint8_t s_hisense_template[] = {
    0x83, 0x06, 0x00, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x40, HISENSE_KEY_TEMPERATURE, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/* Byte 2 bit 2 toggles power. */
static const uint8_t s_hisense_power_template[] = {
    0x83, 0x06, 0x04, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, HISENSE_KEY_POWER, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/* Byte 2 bit 7 and byte 8 bit 6 flag a vane command. */
static const uint8_t s_hisense_swing_template[] = {
    0x83, 0x06, 0x80, 0x82, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x40, HISENSE_KEY_SWING, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const ir_bit_segment_t s_hisense_fan[] = {{2, 0, 2}};
static const ir_bit_segment_t s_hisense_temperature[] = {{3, 4, 4}};
/* Mode nibble, then byte 14 bit 6 that cool sets on every key but power. */
static const ir_bit_segment_t s_hisense_mode[] = {{3, 0, 4}, {14, 6, 1}};
static const ir_bit_segment_t s_hisense_power_mode[] = {{3, 0, 4}};
static const ir_bit_segment_t s_hisense_swing[] = {{10, 5, 3}};
static const ir_bit_segment_t s_hisense_key[] = {{15, 0, 8}};
static const uint32_t s_hisense_fans[] = {0, IR_RAW_NONE, 3, 2, 1};
static const uint32_t s_hisense_temperatures[] = {0xE, 0xF, 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xA, 0xB, 0xC};
#define HISENSE_COOL_FLAG 0x10
static const uint32_t s_hisense_modes[] = {1, 2 | HISENSE_COOL_FLAG, 3, 0, 4};
static const uint32_t s_hisense_power_modes[] = {1, 2, 3, 0, 4};
/* Sweep, then fixed vane positions. */
static const uint32_t s_hisense_swings[] = {IR_RAW_NONE, 0, 1, 2, 3, 4, 5, 6, 7};
/* Temperature down shares code 02 with up and has its own message. */
static const uint32_t s_hisense_keys[] = {IR_RAW_NONE, 0x06, HISENSE_KEY_TEMPERATURE, IR_RAW_NONE, 0x11};
#define HISENSE_STATE_FIELDS(modes_segments, modes) \
    FIELD(IR_FIELD_FAN, s_hisense_fan, s_hisense_fans), \
    FIELD(IR_FIELD_TEMPERATURE, s_hisense_temperature, s_hisense_temperatures, .base = TEMPERATURE_16C, .step = TEMPERATURE_STEP), \
    FIELD(IR_FIELD_MODE, modes_segments, modes), \
    FIELD(IR_FIELD_SWING_V, s_hisense_swing, s_hisense_swings)
static const ir_bitfield_t s_hisense_fields[] = {HISENSE_STATE_FIELDS(s_hisense_mode, s_hisense_modes), FIELD(IR_FIELD_KEY, s_hisense_key, s_hisense_keys)};
static const ir_bitfield_t s_hisense_key_fields[] = {HISENSE_STATE_FIELDS(s_hisense_mode, s_hisense_modes)};
static const ir_bitfield_t s_hisense_power_fields[] = {HISENSE_STATE_FIELDS(s_hisense_power_mode, s_hisense_power_modes)};
static const ir_check_t s_hisense_checks[] = {
    {.op = IR_CHECK_XOR, .first = 2, .count = 11, .target = 13},
    {.op = IR_CHECK_XOR, .first = 14, .count = 6, .target = 20},
};
#define HISENSE_MESSAGE(name_, template_, fields_, implied_) { \
    .name = name_, .layout = &s_hisense_layout, .length = sizeof(template_), .template_bytes = template_, .fields = fields_, \
    .field_count = ARRAY_SIZE(fields_), .checks = s_hisense_checks, .check_count = ARRAY_SIZE(s_hisense_checks), .implied = implied_, \
}
/* Power precedes frame so a power key wins over the power-on frame when both match. */
static const ir_message_t s_hisense_messages[] = {
    HISENSE_MESSAGE("power", s_hisense_power_template, s_hisense_power_fields, &s_key_power),
    HISENSE_MESSAGE("frame", s_hisense_template, s_hisense_fields, &s_power_on),
    HISENSE_MESSAGE("temp_down", s_hisense_template, s_hisense_key_fields, &s_key_temp_down),
    HISENSE_MESSAGE("swing_v", s_hisense_swing_template, s_hisense_key_fields, &s_key_swing),
    HISENSE_MESSAGE("swing_v_fix", s_hisense_swing_template, s_hisense_key_fields, &s_key_swing_fix),
};
#define HISENSE_FIELDS (AC_KEY_FIELDS | IR_FIELD_BIT(IR_FIELD_SWING_V))
#define HISENSE_LAST_POSITION IR_AC_SWING_POSITION(7)
static const ir_protocol_meta_t s_hisense_meta = {
    .category = "air_conditioner", .brand = "Hisense", .model = "Kelon168 family", .source = CAPTURE_SOURCE, .maturity = "experimental",
    .defaults = {.present = HISENSE_FIELDS & ~IR_FIELD_BIT(IR_FIELD_KEY), .value = {IR_AC_DEFAULT_ITEMS, [IR_FIELD_SWING_V] = IR_AC_SWING_POSITION(1)}},
    .limit_fields = HISENSE_FIELDS,
    .limits = {
        IR_AC_LIMIT_ITEMS(16, 30),
        [IR_FIELD_SWING_V] = {.mask = (IR_AC_BIT(HISENSE_LAST_POSITION + 1) - 1) & ~IR_AC_BIT(IR_SWING_OFF)},
        [IR_FIELD_KEY] = {.mask = KEY_MASK(IR_KEY_SWING_V_FIX)},
    },
};

const ir_protocol_t ir_protocol_hisense_kelon168 = {
    .id = "hisense.kelon168", .carrier_hz = 38000, .flags = IR_PROTOCOL_DECODE, .messages = s_hisense_messages,
    .message_count = ARRAY_SIZE(s_hisense_messages), .meta = &s_hisense_meta,
};

/* AUX (Electra family): 13 bytes LSB first, byte sum in the last byte, key code in byte 11. */
static const ir_step_t s_aux_steps[] = {IR_MARK(9000), IR_SPACE(4500), IR_BITS(104), IR_MARK(560)};
static const ir_layout_t s_aux_layout = PAIR_LAYOUT(s_aux_steps, 560, 560, 1690);
static const uint8_t s_aux_template[] = {0xC3, 0x47, 0xE0, 0x00, 0xA0, 0x00, 0x20, 0x00, 0x00, 0x20, 0x00, 0x05, 0x00};
static const ir_bit_segment_t s_aux_power = {9, 5, 1};
static const ir_bit_segment_t s_aux_mode[] = {{6, 5, 3}, {9, 4, 1}};
/* Degrees - 8. */
static const ir_bit_segment_t s_aux_temperature = {1, 3, 5};
#define AUX_TEMPERATURE_BASE (TEMPERATURE_16C - 8 * TEMPERATURE_STEP)
static const ir_bit_segment_t s_aux_fan[] = {{4, 5, 3}};
static const ir_bit_segment_t s_aux_swing[] = {{1, 0, 3}};
static const ir_bit_segment_t s_aux_sleep = {6, 2, 1};
static const ir_bit_segment_t s_aux_key[] = {{11, 0, 8}};
static const uint32_t s_aux_modes[] = {0, 1, 2, 12, 6};
static const uint32_t s_aux_fans[] = {5, IR_RAW_NONE, 3, 2, 1};
static const uint32_t s_aux_swings[] = {7, 0};
static const uint32_t s_aux_keys[] = {
    [IR_KEY_POWER] = 0x05, [IR_KEY_MODE] = 0x06, [IR_KEY_TEMP_UP] = 0x00, [IR_KEY_TEMP_DOWN] = 0x01, [IR_KEY_FAN] = 0x04, [IR_KEY_SWING_V] = 0x02,
    [IR_KEY_SWING_V_FIX] = IR_RAW_NONE, [IR_KEY_SWING_H] = IR_RAW_NONE, [IR_KEY_TURBO] = IR_RAW_NONE, [IR_KEY_QUIET] = IR_RAW_NONE,
    [IR_KEY_ECONO] = IR_RAW_NONE, [IR_KEY_LIGHT] = IR_RAW_NONE, [IR_KEY_SLEEP] = 0x0B,
};
static const ir_bitfield_t s_aux_fields[] = {
    LINEAR_FIELD(IR_FIELD_POWER, s_aux_power),
    FIELD(IR_FIELD_MODE, s_aux_mode, s_aux_modes),
    LINEAR_FIELD(IR_FIELD_TEMPERATURE, s_aux_temperature, .base = AUX_TEMPERATURE_BASE, .step = TEMPERATURE_STEP),
    FIELD(IR_FIELD_FAN, s_aux_fan, s_aux_fans),
    FIELD(IR_FIELD_SWING_V, s_aux_swing, s_aux_swings),
    LINEAR_FIELD(IR_FIELD_SLEEP, s_aux_sleep),
    FIELD(IR_FIELD_KEY, s_aux_key, s_aux_keys),
};
static const ir_check_t s_aux_check = {.op = IR_CHECK_SUM, .first = 0, .count = 12, .target = 12};
static const ir_message_t s_aux_frame = {
    .name = "frame", .layout = &s_aux_layout, .length = sizeof(s_aux_template), .template_bytes = s_aux_template,
    .fields = s_aux_fields, .field_count = ARRAY_SIZE(s_aux_fields), .checks = &s_aux_check, .check_count = 1,
};
#define AUX_FIELDS (AC_KEY_FIELDS | IR_FIELD_BIT(IR_FIELD_SWING_V) | IR_FIELD_BIT(IR_FIELD_SLEEP))
static const ir_protocol_meta_t s_aux_meta = {
    .category = "air_conditioner", .brand = "AUX", .model = "Electra family", .source = CAPTURE_SOURCE, .maturity = "experimental",
    .defaults = {.present = AUX_FIELDS & ~IR_FIELD_BIT(IR_FIELD_KEY), .value = {IR_AC_DEFAULT_ITEMS}},
    .limit_fields = AUX_FIELDS,
    .limits = {
        IR_AC_LIMIT_ITEMS(16, 30),
        [IR_FIELD_SWING_V] = {.mask = SWING_MASK},
        [IR_FIELD_SLEEP] = {.mask = IR_AC_BOOL_MASK},
        [IR_FIELD_KEY] = {.mask = KEY_MASK(IR_KEY_SWING_V) | IR_AC_BIT(IR_KEY_SLEEP)},
    },
};

const ir_protocol_t ir_protocol_aux_electra = {
    .id = "aux.electra", .carrier_hz = 38000, .flags = IR_PROTOCOL_DECODE, .messages = &s_aux_frame, .message_count = 1, .meta = &s_aux_meta,
};

/* TCL (TCL112 family): 14 bytes LSB first, byte sum in the last byte. Byte 8 bit 6 marks the power key. */
static const ir_step_t s_tcl_steps[] = {IR_MARK(3100), IR_SPACE(1650), IR_BITS(112), IR_MARK(400)};
static const ir_layout_t s_tcl_layout = PAIR_LAYOUT(s_tcl_steps, 400, 450, 1150);
static const uint8_t s_tcl_template[] = {0x23, 0xCB, 0x26, 0x01, 0x00, 0x24, 0x03, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t s_tcl_power_template[] = {0x23, 0xCB, 0x26, 0x01, 0x00, 0x24, 0x03, 0x07, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00};
static const ir_bit_segment_t s_tcl_power = {5, 2, 1};
static const ir_bit_segment_t s_tcl_mode[] = {{6, 0, 4}};
static const ir_bit_segment_t s_tcl_temperature[] = {{7, 0, 4}};
static const ir_bit_segment_t s_tcl_fan[] = {{8, 0, 3}};
static const ir_bit_segment_t s_tcl_swing[] = {{8, 3, 3}};
static const uint32_t s_tcl_modes[] = {8, 3, 2, 1, 7};
/* 31 - degrees. */
static const uint32_t s_tcl_temperatures[] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
/* Min is the night speed. */
static const uint32_t s_tcl_fans[] = {0, 1, 2, 3, 5};
static const uint32_t s_tcl_swings[] = {0, 7};
static const ir_bitfield_t s_tcl_fields[] = {
    LINEAR_FIELD(IR_FIELD_POWER, s_tcl_power),
    FIELD(IR_FIELD_MODE, s_tcl_mode, s_tcl_modes),
    FIELD(IR_FIELD_TEMPERATURE, s_tcl_temperature, s_tcl_temperatures, .base = TEMPERATURE_16C, .step = TEMPERATURE_STEP),
    FIELD(IR_FIELD_FAN, s_tcl_fan, s_tcl_fans),
    FIELD(IR_FIELD_SWING_V, s_tcl_swing, s_tcl_swings),
};
static const ir_check_t s_tcl_check = {.op = IR_CHECK_SUM, .first = 0, .count = 13, .target = 13};
#define TCL_MESSAGE(name_, template_, implied_) { \
    .name = name_, .layout = &s_tcl_layout, .length = sizeof(template_), .template_bytes = template_, .fields = s_tcl_fields, \
    .field_count = ARRAY_SIZE(s_tcl_fields), .checks = &s_tcl_check, .check_count = 1, .implied = implied_, \
}
static const ir_message_t s_tcl_messages[] = {
    TCL_MESSAGE("power", s_tcl_power_template, &s_key_power),
    TCL_MESSAGE("frame", s_tcl_template, NULL),
};
#define TCL_FIELDS (AC_KEY_FIELDS | IR_FIELD_BIT(IR_FIELD_SWING_V))
static const ir_protocol_meta_t s_tcl_meta = {
    .category = "air_conditioner", .brand = "TCL", .model = "TCL112 family", .source = CAPTURE_SOURCE, .maturity = "experimental",
    .defaults = {.present = TCL_FIELDS & ~IR_FIELD_BIT(IR_FIELD_KEY), .value = {IR_AC_DEFAULT_ITEMS}},
    .limit_fields = TCL_FIELDS,
    .limits = {
        [IR_FIELD_POWER] = {.mask = IR_AC_BOOL_MASK},
        [IR_FIELD_MODE] = {.mask = IR_AC_MODE_MASK},
        [IR_FIELD_FAN] = {.mask = IR_AC_FAN_MASK | IR_AC_BIT(IR_FAN_MIN)},
        [IR_FIELD_TEMPERATURE] = {.min = IR_AC_DECI(16), .max = IR_AC_DECI(30), .step = IR_AC_DECI(1)},
        [IR_FIELD_SWING_V] = {.mask = SWING_MASK},
        [IR_FIELD_KEY] = {.mask = KEY_MASK(IR_KEY_SWING_V)},
    },
};

const ir_protocol_t ir_protocol_tcl_112 = {
    .id = "tcl.112", .carrier_hz = 38000, .flags = IR_PROTOCOL_DECODE, .messages = s_tcl_messages,
    .message_count = ARRAY_SIZE(s_tcl_messages), .meta = &s_tcl_meta,
};

/*
 * Xiaomi 96-bit: 12 bytes MSB first, two bits per symbol on four space lengths. Bytes 4 and 11 are the XOR of
 * bytes 0-3 and 5-10. Power off is a fixed frame.
 */
#define XIAOMI_MARK_US 580
static const ir_step_t s_xiaomi_steps[] = {IR_MARK(1000), IR_SPACE(600), IR_BITS(96), IR_MARK(XIAOMI_MARK_US)};
static const ir_layout_t s_xiaomi_layout = {
    .coding = IR_CODING_PAIR, .symbol_bits = IR_LAYOUT_SYMBOL_BITS_MAX,
    .symbols = {{XIAOMI_MARK_US, 400}, {XIAOMI_MARK_US, 900}, {XIAOMI_MARK_US, 2230}, {XIAOMI_MARK_US, 1500}},
    .steps = s_xiaomi_steps, .step_count = ARRAY_SIZE(s_xiaomi_steps),
};
#define XIAOMI_HEAD 0xB6, 0x23, 0x00, 0x10, 0x85
/* Byte 5: power << 3 | mode, byte 6: fan, byte 7: degrees << 1, byte 8 bit 3: swing, byte 9: heat bit 3, sleep bit 1. */
static const uint8_t s_xiaomi_template[] = {XIAOMI_HEAD, 0x09, 0x00, 0x34, 0x08, 0x00, 0x00, 0x00};
static const uint8_t s_xiaomi_fan_template[] = {XIAOMI_HEAD, 0x0B, 0x00, 0x30, 0x08, 0x00, 0x00, 0x00};
static const uint8_t s_xiaomi_off_template[] = {XIAOMI_HEAD, 0x01, 0x00, 0x30, 0x00, 0x00, 0x00, 0x31};
/* Mode bits, then the heat bit of byte 9. */
static const ir_bit_segment_t s_xiaomi_mode[] = {{5, 0, 3}, {9, 3, 1}};
static const ir_bit_segment_t s_xiaomi_temperature = {7, 1, 6};
static const ir_bit_segment_t s_xiaomi_fan[] = {{6, 0, 8}};
static const ir_bit_segment_t s_xiaomi_swing[] = {{8, 3, 1}};
static const ir_bit_segment_t s_xiaomi_sleep = {9, 1, 1};
#define XIAOMI_HEAT_FLAG 0x08
static const uint32_t s_xiaomi_modes[] = {IR_RAW_NONE, 1, 2, 4 | XIAOMI_HEAT_FLAG, IR_RAW_NONE};
static const uint32_t s_xiaomi_fans[] = {0x00, IR_RAW_NONE, 0x0A, 0x32, 0x63};
static const uint32_t s_xiaomi_swings[] = {0, 1};
#define XIAOMI_COMMON_FIELDS \
    FIELD(IR_FIELD_FAN, s_xiaomi_fan, s_xiaomi_fans), \
    FIELD(IR_FIELD_SWING_V, s_xiaomi_swing, s_xiaomi_swings), \
    LINEAR_FIELD(IR_FIELD_SLEEP, s_xiaomi_sleep)
static const ir_bitfield_t s_xiaomi_fields[] = {
    FIELD(IR_FIELD_MODE, s_xiaomi_mode, s_xiaomi_modes),
    LINEAR_FIELD(IR_FIELD_TEMPERATURE, s_xiaomi_temperature, .step = TEMPERATURE_STEP),
    XIAOMI_COMMON_FIELDS,
};
/* Fan mode sends a fixed temperature code. */
static const ir_bitfield_t s_xiaomi_fan_fields[] = {XIAOMI_COMMON_FIELDS};
static const ir_check_t s_xiaomi_checks[] = {
    {.op = IR_CHECK_XOR, .first = 0, .count = 4, .target = 4},
    {.op = IR_CHECK_XOR, .first = 5, .count = 6, .target = 11},
};
#define XIAOMI_MESSAGE(name_, template_, fields_, field_count_, implied_) { \
    .name = name_, .layout = &s_xiaomi_layout, .msb_first = true, .length = sizeof(template_), .template_bytes = template_, \
    .fields = fields_, .field_count = field_count_, .checks = s_xiaomi_checks, .check_count = ARRAY_SIZE(s_xiaomi_checks), \
    .implied = implied_, \
}
static const ir_message_t s_xiaomi_messages[] = {
    XIAOMI_MESSAGE("fan", s_xiaomi_fan_template, s_xiaomi_fan_fields, ARRAY_SIZE(s_xiaomi_fan_fields), &s_mode_fan),
    XIAOMI_MESSAGE("frame", s_xiaomi_template, s_xiaomi_fields, ARRAY_SIZE(s_xiaomi_fields), &s_power_on),
    XIAOMI_MESSAGE("off", s_xiaomi_off_template, NULL, 0, &s_power_off),
};
#define XIAOMI_FIELDS (IR_AC_FIELDS | IR_FIELD_BIT(IR_FIELD_SWING_V) | IR_FIELD_BIT(IR_FIELD_SLEEP))
static const ir_limit_t s_xiaomi_dry_limits[IR_FIELD_COUNT] = {[IR_FIELD_FAN] = {.mask = IR_AC_BIT(IR_FAN_LOW)}};
static const ir_protocol_meta_t s_xiaomi_meta = {
    .category = "air_conditioner", .brand = "Xiaomi", .model = "96-bit", .source = CAPTURE_SOURCE, .maturity = "experimental",
    .defaults = {.present = XIAOMI_FIELDS, .value = {IR_AC_DEFAULT_ITEMS, [IR_FIELD_SWING_V] = IR_SWING_AUTO}},
    .limit_fields = XIAOMI_FIELDS,
    .limits = {
        [IR_FIELD_POWER] = {.mask = IR_AC_BOOL_MASK},
        [IR_FIELD_MODE] = {.mask = IR_AC_MODE_MASK & ~IR_AC_BIT(IR_MODE_AUTO)},
        [IR_FIELD_FAN] = {.mask = IR_AC_FAN_MASK},
        [IR_FIELD_TEMPERATURE] = {.min = IR_AC_DECI(16), .max = IR_AC_DECI(30), .step = IR_AC_DECI(1)},
        [IR_FIELD_SWING_V] = {.mask = SWING_MASK},
        [IR_FIELD_SLEEP] = {.mask = IR_AC_BOOL_MASK},
    },
    .mode_limit_fields = {[IR_MODE_DRY] = IR_FIELD_BIT(IR_FIELD_FAN), [IR_MODE_FAN] = NO_TEMPERATURE},
    .mode_limits = {[IR_MODE_DRY] = s_xiaomi_dry_limits, [IR_MODE_FAN] = s_no_temperature_limits},
};

const ir_protocol_t ir_protocol_xiaomi_96 = {
    .id = "xiaomi.96", .carrier_hz = 38000, .flags = IR_PROTOCOL_DECODE, .messages = s_xiaomi_messages,
    .message_count = ARRAY_SIZE(s_xiaomi_messages), .meta = &s_xiaomi_meta,
};
