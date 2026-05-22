/*
 * ESP-IDF v6 compatibility shim for M5Unified i2s_port_t.
 *
 * In ESP-IDF v6, i2s_port_t was removed. M5Unified 0.2.x still uses it.
 * This header:
 *   1. Includes driver/i2s_std.h (so its include guards fire on subsequent
 *      inclusions by Speaker_Class.hpp / Mic_Class.hpp).
 *   2. Undefines the I2S_NUM_x macros that i2s_std.h defines.
 *   3. Defines i2s_port_t as a C enum with the same values.
 *
 * Result: Speaker_Class.hpp compiles because i2s_port_t::I2S_NUM_0 resolves
 * to the enum value, not to a preprocessor macro expansion.
 */
#pragma once

#if __has_include(<driver/i2s_std.h>)
#include <driver/i2s_std.h>

#ifndef __i2s_port_compat__
#define __i2s_port_compat__
#ifdef I2S_NUM_0
#undef I2S_NUM_0
#endif
#ifdef I2S_NUM_1
#undef I2S_NUM_1
#endif
#ifdef I2S_NUM_2
#undef I2S_NUM_2
#endif
#ifdef I2S_NUM_AUTO
#undef I2S_NUM_AUTO
#endif
#ifdef I2S_NUM_MAX
#undef I2S_NUM_MAX
#endif
typedef enum {
    I2S_NUM_0    = 0,
    I2S_NUM_1    = 1,
    I2S_NUM_MAX  = 2,
    I2S_NUM_AUTO = -1,
} i2s_port_t;
#endif /* __i2s_port_compat__ */

#endif /* __has_include(<driver/i2s_std.h>) */
