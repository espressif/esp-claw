/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_color.h"

uint16_t display_color_to_rgb565(display_color_t color)
{
    return (uint16_t)(((color.r & 0xF8) << 8) | ((color.g & 0xFC) << 3) | ((color.b & 0xF8) >> 3));
}

/* Exact x/255 for x in [0..65534] without a hardware divide. */
static inline uint8_t display_color_div255(uint32_t x)
{
    return (uint8_t)((x * 0x8081U) >> 23);
}

uint16_t display_color_blend_rgb565(uint16_t dst, display_color_t src)
{
    if (src.a == 0) return dst;
    if (src.a == 255) return display_color_to_rgb565(src);
    uint32_t a = src.a, inv = 255U - a;
    uint32_t red = display_color_div255((src.r >> 3) * a + (dst >> 11) * inv + 127U);
    uint32_t green = display_color_div255((src.g >> 2) * a + ((dst >> 5) & 63U) * inv + 127U);
    uint32_t blue = display_color_div255((src.b >> 3) * a + (dst & 31U) * inv + 127U);
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

uint32_t display_color_to_rgb888(display_color_t color)
{
    return ((uint32_t)color.b << 16) | ((uint32_t)color.g << 8) | (uint32_t)color.r;
}

uint32_t display_color_blend_rgb888(uint32_t dst, display_color_t src)
{
    if (src.a == 0) return dst;
    if (src.a == 255) return display_color_to_rgb888(src);
    uint32_t a = src.a, inv_a = 255U - a;
    uint32_t dst_b = (dst >> 16) & 0xFFU, dst_g = (dst >> 8) & 0xFFU, dst_r = dst & 0xFFU;
    /* +127 for round-to-nearest. */
    uint32_t b = display_color_div255((uint32_t)src.b * a + dst_b * inv_a + 127U);
    uint32_t g = display_color_div255((uint32_t)src.g * a + dst_g * inv_a + 127U);
    uint32_t r = display_color_div255((uint32_t)src.r * a + dst_r * inv_a + 127U);
    return (b << 16) | (g << 8) | r;
}

bool display_color_is_transparent(display_color_t color) { return color.a == 0; }
bool display_color_is_opaque(display_color_t color) { return color.a == 255; }
