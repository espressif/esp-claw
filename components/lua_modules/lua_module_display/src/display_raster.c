/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }

display_raster_pen_t display_raster_make_pen(display_color_t color)
{
    return (display_raster_pen_t) {
        .color = color,
        .rgb565 = display_color_to_rgb565(color),
        .rgb888 = display_color_to_rgb888(color),
    };
}

static void write_rgb565_opaque(uint16_t *dst, uint16_t value, size_t count)
{
    if ((uint8_t)value == (uint8_t)(value >> 8)) {
        memset(dst, (uint8_t)value, count * sizeof(*dst));
        return;
    }
    if (((uintptr_t)dst & 3U) && count) {
        *dst++ = value;
        count--;
    }
    uint32_t pair = (uint32_t)value | ((uint32_t)value << 16);
    uint32_t *dst32 = (uint32_t *)dst;
    for (size_t i = 0; i < count / 2; ++i) dst32[i] = pair;
    if (count & 1U) ((uint16_t *)(dst32 + count / 2))[0] = value;
}

static void write_run(display_raster_t *r, uint8_t *dst, size_t count, const display_raster_pen_t *pen)
{
    if (r->bpp == 2) {
        uint16_t *pixels = (uint16_t *)dst;
        if (pen->color.a == 255) {
            write_rgb565_opaque(pixels, pen->rgb565, count);
        } else {
            for (size_t i = 0; i < count; ++i) pixels[i] = display_color_blend_rgb565(pixels[i], pen->color);
        }
        return;
    }
    if (pen->color.a == 255 && pen->color.r == pen->color.g && pen->color.g == pen->color.b) {
        memset(dst, pen->color.r, count * 3);
        return;
    }
    for (size_t i = 0; i < count; ++i, dst += 3) {
        uint32_t value = pen->color.a == 255 ? pen->rgb888 :
                         display_color_blend_rgb888(((uint32_t)dst[0] << 16) | ((uint32_t)dst[1] << 8) | dst[2], pen->color);
        dst[0] = value >> 16;
        dst[1] = value >> 8;
        dst[2] = value;
    }
}

static inline void write_pixel_prepared(display_raster_t *r, uint8_t *dst, const display_raster_pen_t *pen)
{
    if (r->bpp == 2) {
        uint16_t *pixel = (uint16_t *)dst;
        *pixel = pen->color.a == 255 ? pen->rgb565 : display_color_blend_rgb565(*pixel, pen->color);
        return;
    }
    uint32_t value = pen->rgb888;
    if (pen->color.a != 255) {
        uint32_t background = ((uint32_t)dst[0] << 16) | ((uint32_t)dst[1] << 8) | dst[2];
        value = display_color_blend_rgb888(background, pen->color);
    }
    dst[0] = value >> 16;
    dst[1] = value >> 8;
    dst[2] = value;
}

static void mark_local_bounds(display_raster_t *r, int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    x0 = max64(x0 + r->tx, r->x0);
    y0 = max64(y0 + r->ty, r->y0);
    x1 = min64(x1 + r->tx, r->x1);
    y1 = min64(y1 + r->ty, r->y1);
    if (x0 < x1 && y0 < y1) display_dirty_mark(r->dirty, (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0));
}

void display_raster_run_unchecked(display_raster_t *r, int x, int y, size_t count, const display_raster_pen_t *pen)
{
    write_run(r, r->pixels + (size_t)y * r->stride + (size_t)x * r->bpp, count, pen);
}

static bool clip_span(display_raster_t *r, int64_t *x0, int64_t *x1, int64_t *y)
{
    *y += r->ty;
    if (*y < r->y0 || *y >= r->y1) return false;
    *x0 = max64(*x0 + r->tx, r->x0);
    *x1 = min64(*x1 + r->tx, r->x1);
    return *x0 < *x1;
}

static void draw_span(display_raster_t *r, int64_t x0, int64_t x1, int64_t y, const display_raster_pen_t *pen, bool mark_dirty)
{
    if (!clip_span(r, &x0, &x1, &y)) return;
    uint8_t *dst = r->pixels + (size_t)y * r->stride + (size_t)x0 * r->bpp;
    write_run(r, dst, (size_t)(x1 - x0), pen);
    if (mark_dirty) display_dirty_mark(r->dirty, (int)x0, (int)y, (int)(x1 - x0), 1);
}

void display_raster_span(display_raster_t *r, int64_t x0, int64_t x1, int64_t y, display_color_t c)
{
    if (!c.a) return;
    display_raster_pen_t pen = display_raster_make_pen(c);
    draw_span(r, x0, x1, y, &pen, true);
}

void display_raster_pixel(display_raster_t *r, int64_t x, int64_t y, display_color_t c)
{
    display_raster_span(r, x, x + 1, y, c);
}

void display_raster_fill_rect(display_raster_t *r, int x, int y, int w, int h, display_color_t c)
{
    if (w <= 0 || h <= 0 || !c.a) return;
    int64_t left = max64((int64_t)x + r->tx, r->x0);
    int64_t top = max64((int64_t)y + r->ty, r->y0);
    int64_t right = min64((int64_t)x + w + r->tx, r->x1);
    int64_t bottom = min64((int64_t)y + h + r->ty, r->y1);
    if (left >= right || top >= bottom) return;
    display_raster_pen_t pen = display_raster_make_pen(c);
    size_t width = (size_t)(right - left);
    uint8_t *dst = r->pixels + (size_t)top * r->stride + (size_t)left * r->bpp;
    if (width * (size_t)r->bpp == r->stride) {
        write_run(r, dst, width * (size_t)(bottom - top), &pen);
    } else {
        for (int64_t row = top; row < bottom; ++row, dst += r->stride) write_run(r, dst, width, &pen);
    }
    display_dirty_mark(r->dirty, (int)left, (int)top, (int)width, (int)(bottom - top));
}

void display_raster_stroke_rect(display_raster_t *r, int x, int y, int w, int h, display_color_t c)
{
    if (w <= 0 || h <= 0 || !c.a) return;
    display_raster_pen_t pen = display_raster_make_pen(c);
    draw_span(r, x, (int64_t)x + w, y, &pen, false);
    if (h > 1) draw_span(r, x, (int64_t)x + w, (int64_t)y + h - 1, &pen, false);
    int64_t bottom = min64((int64_t)y + h - 1, (int64_t)r->y1 - r->ty);
    int64_t left = (int64_t)x + r->tx, right = (int64_t)x + w - 1 + r->tx;
    for (int64_t row = max64((int64_t)y + 1, (int64_t)r->y0 - r->ty); row < bottom; row++) {
        int64_t screen_y = row + r->ty;
        if (left >= r->x0 && left < r->x1) write_pixel_prepared(r, r->pixels + (size_t)screen_y * r->stride + (size_t)left * r->bpp, &pen);
        if (w > 1 && right >= r->x0 && right < r->x1) write_pixel_prepared(r, r->pixels + (size_t)screen_y * r->stride + (size_t)right * r->bpp, &pen);
    }
    mark_local_bounds(r, x, y, (int64_t)x + w, (int64_t)y + h);
}

static bool clip_edge(double p, double q, double *lo, double *hi)
{
    if (p == 0) return q >= 0;
    double t = q / p;
    if (p < 0) {
        if (t > *hi) return false;
        if (t > *lo) *lo = t;
    } else {
        if (t < *lo) return false;
        if (t < *hi) *hi = t;
    }
    return true;
}

void display_raster_line(display_raster_t *r, int x0, int y0, int x1, int y1, display_color_t c)
{
    if (!c.a || r->x0 >= r->x1 || r->y0 >= r->y1) return;
    int64_t ax = (int64_t)x0 + r->tx, ay = (int64_t)y0 + r->ty;
    int64_t bx = (int64_t)x1 + r->tx, by = (int64_t)y1 + r->ty;
    if (ax < r->x0 || ax >= r->x1 || bx < r->x0 || bx >= r->x1 ||
        ay < r->y0 || ay >= r->y1 || by < r->y0 || by >= r->y1) {
        /* Clip before stepping so off-screen coordinates cannot create unbounded work. */
        double dx = (double)x1 - x0, dy = (double)y1 - y0, lo = 0, hi = 1;
        if (!clip_edge(-dx, (double)x0 + r->tx - r->x0, &lo, &hi) ||
            !clip_edge(dx, (double)r->x1 - 1 - r->tx - x0, &lo, &hi) ||
            !clip_edge(-dy, (double)y0 + r->ty - r->y0, &lo, &hi) ||
            !clip_edge(dy, (double)r->y1 - 1 - r->ty - y0, &lo, &hi)) return;
        ax = llround(x0 + lo * dx) + r->tx;
        ay = llround(y0 + lo * dy) + r->ty;
        bx = llround(x0 + hi * dx) + r->tx;
        by = llround(y0 + hi * dy) + r->ty;
        ax = max64(r->x0, min64(ax, r->x1 - 1));
        bx = max64(r->x0, min64(bx, r->x1 - 1));
        ay = max64(r->y0, min64(ay, r->y1 - 1));
        by = max64(r->y0, min64(by, r->y1 - 1));
    }
    display_raster_pen_t pen = display_raster_make_pen(c);
    if (ay == by) {
        int64_t left = min64(ax, bx), right = max64(ax, bx) + 1;
        uint8_t *dst = r->pixels + (size_t)ay * r->stride + (size_t)left * r->bpp;
        write_run(r, dst, (size_t)(right - left), &pen);
        display_dirty_mark(r->dirty, (int)left, (int)ay, (int)(right - left), 1);
        return;
    }
    int64_t sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1;
    int64_t adx = llabs(bx - ax), ady = -llabs(by - ay), err = adx + ady;
    int64_t left = min64(ax, bx), top = min64(ay, by), right = max64(ax, bx), bottom = max64(ay, by);
    for (;;) {
        uint8_t *dst = r->pixels + (size_t)ay * r->stride + (size_t)ax * r->bpp;
        write_pixel_prepared(r, dst, &pen);
        if (ax == bx && ay == by) break;
        int64_t e = err * 2;
        if (e >= ady) { err += ady; ax += sx; }
        if (e <= adx) { err += adx; ay += sy; }
    }
    display_dirty_mark(r->dirty, (int)left, (int)top, (int)(right - left + 1), (int)(bottom - top + 1));
}

typedef struct {
    int radius;
    int64_t dy;
    int64_t x;
    int64_t limit;
    int64_t squared;
    bool ready;
} circle_cursor_t;

static int64_t circle_extent(int radius, int64_t dy)
{
    int64_t n = (int64_t)radius * radius - dy * dy;
    if (n < 0) return -1;
    int64_t x = (int64_t)sqrt((double)n);
    while (x * x > n) x--;
    while ((x + 1) * (x + 1) <= n) x++;
    return x;
}

static int64_t circle_cursor_extent(circle_cursor_t *cursor, int radius, int64_t dy)
{
    if (dy < 0) dy = -dy;
    if (dy > radius) return -1;
    if (!cursor->ready || cursor->radius != radius) {
        cursor->radius = radius;
        cursor->x = circle_extent(radius, dy);
        cursor->limit = (int64_t)radius * radius - dy * dy;
        cursor->squared = cursor->x * cursor->x;
        cursor->ready = true;
    } else {
        if (dy == cursor->dy + 1) cursor->limit -= cursor->dy * 2 + 1;
        else if (cursor->dy == dy + 1) cursor->limit += dy * 2 + 1;
        else cursor->limit = (int64_t)radius * radius - dy * dy;
        while (cursor->squared > cursor->limit) { cursor->squared -= cursor->x * 2 - 1; cursor->x--; }
        while (cursor->squared + cursor->x * 2 + 1 <= cursor->limit) { cursor->squared += cursor->x * 2 + 1; cursor->x++; }
    }
    cursor->dy = dy;
    return cursor->x;
}

static void circle(display_raster_t *r, int cx, int cy, int radius, bool fill, display_color_t c)
{
    if (radius < 0 || !c.a) return;
    display_raster_pen_t pen = display_raster_make_pen(c);
    circle_cursor_t outer_cursor = {0}, inner_cursor = {0};
    int64_t bottom = min64((int64_t)cy + radius, (int64_t)r->y1 - r->ty - 1);
    for (int64_t y = max64((int64_t)cy - radius, (int64_t)r->y0 - r->ty); y <= bottom; y++) {
        int64_t outer = circle_cursor_extent(&outer_cursor, radius, y - cy);
        int64_t inner = !fill && radius > 0 ? circle_cursor_extent(&inner_cursor, radius - 1, y - cy) : -1;
        if (inner < 0) draw_span(r, (int64_t)cx - outer, (int64_t)cx + outer + 1, y, &pen, false);
        else {
            draw_span(r, (int64_t)cx - outer, (int64_t)cx - inner, y, &pen, false);
            draw_span(r, (int64_t)cx + inner + 1, (int64_t)cx + outer + 1, y, &pen, false);
        }
    }
    int64_t left = max64((int64_t)cx - radius + r->tx, r->x0);
    int64_t top = max64((int64_t)cy - radius + r->ty, r->y0);
    int64_t right = min64((int64_t)cx + radius + 1 + r->tx, r->x1);
    int64_t dirty_bottom = min64((int64_t)cy + radius + 1 + r->ty, r->y1);
    if (left < right && top < dirty_bottom) display_dirty_mark(r->dirty, (int)left, (int)top, (int)(right - left), (int)(dirty_bottom - top));
}

void display_raster_fill_circle(display_raster_t *r, int cx, int cy, int radius, display_color_t c)
{
    circle(r, cx, cy, radius, true, c);
}

void display_raster_stroke_circle(display_raster_t *r, int cx, int cy, int radius, display_color_t c)
{
    circle(r, cx, cy, radius, false, c);
}

void display_raster_arc(display_raster_t *r, int cx, int cy, int radius, double start, double end, display_color_t c)
{
    if (radius < 0 || !c.a || !isfinite(start) || !isfinite(end)) return;
    double sweep = end - start;
    if (fabs(sweep) >= 360) { circle(r, cx, cy, radius, false, c); return; }
    sweep = fmod(sweep + 360, 360);
    if (sweep == 0) return;
    const double radians = 0.017453292519943295;
    start = fmod(start, 360) * radians;
    double finish = start + sweep * radians;
    double sx = cos(start), sy = sin(start), ex = cos(finish), ey = sin(finish);
    display_raster_pen_t pen = display_raster_make_pen(c);
    circle_cursor_t outer_cursor = {0}, inner_cursor = {0};
    int64_t dirty_left = INT64_MAX, dirty_top = INT64_MAX, dirty_right = INT64_MIN, dirty_bottom = INT64_MIN;
    int64_t bottom = min64((int64_t)cy + radius, (int64_t)r->y1 - r->ty - 1);
    for (int64_t y = max64((int64_t)cy - radius, (int64_t)r->y0 - r->ty); y <= bottom; y++) {
        int64_t outer = circle_cursor_extent(&outer_cursor, radius, y - cy);
        int64_t inner = radius > 0 ? circle_cursor_extent(&inner_cursor, radius - 1, y - cy) : -1;
        int64_t left = max64((int64_t)cx - outer, (int64_t)r->x0 - r->tx);
        int64_t right = min64((int64_t)cx + outer, (int64_t)r->x1 - r->tx - 1);
        for (int64_t x = left; x <= right; x++) {
            if (inner >= 0 && x >= (int64_t)cx - inner && x <= (int64_t)cx + inner) {
                x = (int64_t)cx + inner;
                continue;
            }
            double dx = x - cx, dy = y - cy;
            bool after = sx * dy - sy * dx >= -1e-9;
            bool before = dx * ey - dy * ex >= -1e-9;
            if (sweep <= 180 ? (after && before) : (after || before)) {
                int64_t screen_x = x + r->tx, screen_y = y + r->ty;
                write_pixel_prepared(r, r->pixels + (size_t)screen_y * r->stride + (size_t)screen_x * r->bpp, &pen);
                dirty_left = min64(dirty_left, screen_x);
                dirty_top = min64(dirty_top, screen_y);
                dirty_right = max64(dirty_right, screen_x);
                dirty_bottom = max64(dirty_bottom, screen_y);
            }
        }
    }
    if (dirty_left <= dirty_right) display_dirty_mark(r->dirty, (int)dirty_left, (int)dirty_top, (int)(dirty_right - dirty_left + 1), (int)(dirty_bottom - dirty_top + 1));
}

static void round_rect(display_raster_t *r, int x, int y, int w, int h, int radius, bool fill, display_color_t c)
{
    if (w <= 0 || h <= 0 || radius < 0 || !c.a) return;
    radius = (int)min64(radius, min64((w - 1) / 2, (h - 1) / 2));
    display_raster_pen_t pen = display_raster_make_pen(c);
    circle_cursor_t outer_cursor = {0}, inner_cursor = {0};
    int64_t bottom = min64((int64_t)y + h, (int64_t)r->y1 - r->ty);
    for (int64_t row = max64(y, (int64_t)r->y0 - r->ty); row < bottom; row++) {
        int64_t relative = row - y;
        int64_t dy = relative < radius ? radius - relative : relative - (h - radius - 1);
        int64_t inset = dy > 0 ? radius - circle_cursor_extent(&outer_cursor, radius, dy) : 0;
        int64_t left = (int64_t)x + inset, right = (int64_t)x + w - inset;
        if (fill || row == y || row == (int64_t)y + h - 1 || w <= 2 || h <= 2) {
            draw_span(r, left, right, row, &pen, false);
        } else {
            int inner_radius = radius > 0 ? radius - 1 : 0;
            int64_t inner_relative = relative - 1;
            int64_t inner_dy = inner_relative < inner_radius ? inner_radius - inner_relative : inner_relative - (h - 2 - inner_radius - 1);
            int64_t inner = 1 + (inner_dy > 0 ? inner_radius - circle_cursor_extent(&inner_cursor, inner_radius, inner_dy) : 0);
            draw_span(r, left, (int64_t)x + inner, row, &pen, false);
            draw_span(r, (int64_t)x + w - inner, right, row, &pen, false);
        }
    }
    mark_local_bounds(r, x, y, (int64_t)x + w, (int64_t)y + h);
}

void display_raster_fill_round_rect(display_raster_t *r, int x, int y, int w, int h, int radius, display_color_t c)
{
    round_rect(r, x, y, w, h, radius, true, c);
}

void display_raster_stroke_round_rect(display_raster_t *r, int x, int y, int w, int h, int radius, display_color_t c)
{
    round_rect(r, x, y, w, h, radius, false, c);
}

typedef struct {
    int64_t x;
    int64_t step;
    uint64_t remainder;
    uint64_t step_remainder;
    uint64_t denominator;
} triangle_edge_t;

static triangle_edge_t triangle_edge(int x0, int y0, int x1, int y1, int64_t first_y)
{
    int64_t dx = (int64_t)x1 - x0;
    int64_t dy = (int64_t)y1 - y0;
    int64_t step = dx / dy;
    int64_t step_remainder = dx % dy;
    if (step_remainder < 0) { step--; step_remainder += dy; }
    uint64_t delta = (uint64_t)(first_y - y0);
    uint64_t accumulated = delta * (uint64_t)step_remainder;
    return (triangle_edge_t) {
        .x = (int64_t)x0 + (int64_t)delta * step + (int64_t)(accumulated / (uint64_t)dy),
        .step = step,
        .remainder = accumulated % (uint64_t)dy,
        .step_remainder = (uint64_t)step_remainder,
        .denominator = (uint64_t)dy,
    };
}

static void triangle_edge_advance(triangle_edge_t *edge)
{
    edge->x += edge->step;
    edge->remainder += edge->step_remainder;
    if (edge->remainder >= edge->denominator) { edge->remainder -= edge->denominator; edge->x++; }
}

static bool triangle_edge_before(const triangle_edge_t *a, const triangle_edge_t *b)
{
    if (a->x != b->x) return a->x < b->x;
    return a->remainder * b->denominator < b->remainder * a->denominator;
}

static void fill_triangle_half(display_raster_t *r, int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1,
                               int64_t first_y, int64_t last_y, const display_raster_pen_t *pen)
{
    first_y = max64(first_y, (int64_t)r->y0 - r->ty);
    last_y = min64(last_y, (int64_t)r->y1 - r->ty - 1);
    if (first_y > last_y) return;
    triangle_edge_t edge_a = triangle_edge(ax0, ay0, ax1, ay1, first_y);
    triangle_edge_t edge_b = triangle_edge(bx0, by0, bx1, by1, first_y);
    for (int64_t y = first_y; y <= last_y; ++y) {
        const triangle_edge_t *left = triangle_edge_before(&edge_a, &edge_b) ? &edge_a : &edge_b;
        const triangle_edge_t *right = left == &edge_a ? &edge_b : &edge_a;
        draw_span(r, left->x + (left->remainder != 0), right->x + 1, y, pen, false);
        triangle_edge_advance(&edge_a);
        triangle_edge_advance(&edge_b);
    }
}

void display_raster_fill_triangle(display_raster_t *r, int x0, int y0, int x1, int y1, int x2, int y2, display_color_t c)
{
    if (!c.a) return;
    struct { int x, y; } points[3] = {{x0, y0}, {x1, y1}, {x2, y2}};
    for (int i = 1; i < 3; ++i) {
        for (int j = i; j > 0 && points[j].y < points[j - 1].y; --j) {
            int tx = points[j].x, ty = points[j].y;
            points[j] = points[j - 1];
            points[j - 1].x = tx; points[j - 1].y = ty;
        }
    }
    display_raster_pen_t pen = display_raster_make_pen(c);
    if (points[0].y == points[2].y) {
        int left = points[0].x, right = points[0].x;
        for (int i = 1; i < 3; ++i) { if (points[i].x < left) left = points[i].x; if (points[i].x > right) right = points[i].x; }
        draw_span(r, left, (int64_t)right + 1, points[0].y, &pen, false);
    } else if (points[0].y == points[1].y) {
        fill_triangle_half(r, points[0].x, points[0].y, points[2].x, points[2].y, points[1].x, points[1].y, points[2].x, points[2].y,
                           points[0].y, points[2].y, &pen);
    } else if (points[1].y == points[2].y) {
        fill_triangle_half(r, points[0].x, points[0].y, points[1].x, points[1].y, points[0].x, points[0].y, points[2].x, points[2].y,
                           points[0].y, points[2].y, &pen);
    } else {
        fill_triangle_half(r, points[0].x, points[0].y, points[1].x, points[1].y, points[0].x, points[0].y, points[2].x, points[2].y,
                           points[0].y, points[1].y, &pen);
        fill_triangle_half(r, points[1].x, points[1].y, points[2].x, points[2].y, points[0].x, points[0].y, points[2].x, points[2].y,
                           (int64_t)points[1].y + 1, points[2].y, &pen);
    }
    mark_local_bounds(r, min64(x0, min64(x1, x2)), min64(y0, min64(y1, y2)), max64(x0, max64(x1, x2)) + 1, max64(y0, max64(y1, y2)) + 1);
}
