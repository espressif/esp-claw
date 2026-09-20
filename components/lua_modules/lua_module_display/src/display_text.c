/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_text.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_painter_font.h"

#define FONT_MAX_DIMENSION 64
#define FONT_MIN_SIZE 8
#define FONT_MAX_GLYPHS 4096
#define FONT_MAX_BYTES (1024 * 1024)

static const char *TAG = "display_text";

static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }

struct display_font_t {
    uint16_t width, height;
    uint32_t count;
    size_t record_size;
    uint8_t records[];
};

typedef struct {
    const esp_painter_basic_font_t *builtin;
    display_font_handle_t custom;
    int width, height;
    int source_width, source_height;
    uint8_t columns[FONT_MAX_DIMENSION];
} font_view_t;

static uint32_t read32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool valid_codepoint(uint32_t cp)
{
    return cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
}

esp_err_t display_font_create(const char *path, display_font_handle_t *ret_font)
{
    if (!path || !ret_font) return ESP_ERR_INVALID_ARG;
    *ret_font = NULL;
    FILE *file = fopen(path, "rb");
    if (!file) { ESP_LOGE(TAG, "font open failed"); return ESP_ERR_NOT_FOUND; }
    esp_err_t err = ESP_ERR_INVALID_SIZE;
    display_font_handle_t font = NULL;
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "DFN1", 4)) goto done;
    unsigned width = header[4] | ((unsigned)header[5] << 8);
    unsigned height = header[6] | ((unsigned)header[7] << 8);
    uint32_t count = read32(header + 8);
    if (!width || width > FONT_MAX_DIMENSION || !height || height > FONT_MAX_DIMENSION || !count || count > FONT_MAX_GLYPHS) goto done;
    size_t record_size = 4 + ((width + 7) / 8) * height;
    size_t bytes = record_size * count;
    if (bytes > FONT_MAX_BYTES - sizeof(header)) goto done;
    font = malloc(sizeof(*font) + bytes);
    if (!font) { err = ESP_ERR_NO_MEM; goto done; }
    font->width = width;
    font->height = height;
    font->count = count;
    font->record_size = record_size;
    if (fread(font->records, 1, bytes, file) != bytes || fgetc(file) != EOF || ferror(file)) goto done;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t cp = read32(font->records + i * record_size);
        if (!valid_codepoint(cp) || (i && cp <= previous)) goto done;
        previous = cp;
    }
    err = ESP_OK;
done:
    fclose(file);
    if (err != ESP_OK) {
        free(font);
        ESP_LOGE(TAG, "font load failed: %s", esp_err_to_name(err));
    } else *ret_font = font;
    return err;
}

void display_font_delete(display_font_handle_t font)
{
    free(font);
}

static const esp_painter_basic_font_t *builtin_font(void)
{
#if CONFIG_ESP_PAINTER_BASIC_FONT_24
    return &esp_painter_basic_font_24;
#elif CONFIG_ESP_PAINTER_BASIC_FONT_20
    return &esp_painter_basic_font_20;
#elif CONFIG_ESP_PAINTER_BASIC_FONT_16
    return &esp_painter_basic_font_16;
#elif CONFIG_ESP_PAINTER_BASIC_FONT_12
    return &esp_painter_basic_font_12;
#elif CONFIG_ESP_PAINTER_BASIC_FONT_28
    return &esp_painter_basic_font_28;
#elif CONFIG_ESP_PAINTER_BASIC_FONT_32
    return &esp_painter_basic_font_32;
#elif CONFIG_ESP_PAINTER_BASIC_FONT_36
    return &esp_painter_basic_font_36;
#elif CONFIG_ESP_PAINTER_BASIC_FONT_40
    return &esp_painter_basic_font_40;
#elif CONFIG_ESP_PAINTER_BASIC_FONT_44
    return &esp_painter_basic_font_44;
#elif CONFIG_ESP_PAINTER_BASIC_FONT_48
    return &esp_painter_basic_font_48;
#else
    return NULL;
#endif
}

static esp_err_t font_view(const display_text_options_t *options, font_view_t *view)
{
    view->custom = options->font;
    view->builtin = options->font ? NULL : builtin_font();
    if (!view->custom && !view->builtin) return ESP_ERR_NOT_SUPPORTED;
    view->source_width = view->custom ? view->custom->width : view->builtin->width;
    view->source_height = view->custom ? view->custom->height : view->builtin->height;
    view->width = view->source_width;
    view->height = view->source_height;
    if (!view->custom) {
        if (options->font_size < FONT_MIN_SIZE || options->font_size > FONT_MAX_DIMENSION) return ESP_ERR_INVALID_ARG;
        view->height = options->font_size;
        view->width = (view->source_width * view->height + view->source_height / 2) / view->source_height;
        if (view->width < 1) view->width = 1;
    }
    if (view->width > FONT_MAX_DIMENSION) return ESP_ERR_INVALID_SIZE;
    /* Reuse the horizontal mapping for every glyph, with no per-pixel division. */
    for (int x = 0; x < view->width; x++) view->columns[x] = x * view->source_width / view->width;
    return ESP_OK;
}

static const uint8_t *find_glyph(display_font_handle_t font, uint32_t cp)
{
    uint32_t lo = 0, hi = font->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *record = font->records + mid * font->record_size;
        uint32_t code = read32(record);
        if (code == cp) return record + 4;
        if (code < cp) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static const uint8_t *glyph(const font_view_t *font, uint32_t cp)
{
    if (font->builtin) {
        if (cp < 32 || cp > 126) return NULL;
        return font->builtin->bitmap + (cp - 32) * ((font->source_width + 7) / 8) * font->source_height;
    }
    const uint8_t *bitmap = find_glyph(font->custom, cp);
    return bitmap ? bitmap : find_glyph(font->custom, '?');
}

static bool next_codepoint(const char *text, size_t length, size_t *offset, uint32_t *cp)
{
    uint8_t lead = (uint8_t)text[(*offset)++];
    if (lead < 0x80) { *cp = lead; return true; }
    int count;
    uint32_t minimum;
    if (lead >= 0xC2 && lead <= 0xDF) { count = 1; *cp = lead & 31; minimum = 0x80; }
    else if (lead >= 0xE0 && lead <= 0xEF) { count = 2; *cp = lead & 15; minimum = 0x800; }
    else if (lead >= 0xF0 && lead <= 0xF4) { count = 3; *cp = lead & 7; minimum = 0x10000; }
    else return false;
    if (length - *offset < (size_t)count) return false;
    for (int i = 0; i < count; i++) {
        uint8_t ch = (uint8_t)text[(*offset)++];
        if ((ch & 0xC0) != 0x80) return false;
        *cp = (*cp << 6) | (ch & 63);
    }
    return *cp >= minimum && valid_codepoint(*cp);
}

static void draw_glyph(display_raster_t *r, int64_t x, int64_t y, const uint8_t *bitmap, const font_view_t *font,
                       const uint8_t *rows, const display_raster_pen_t *pen)
{
    int64_t clip_left = (int64_t)r->x0 - r->tx, clip_top = (int64_t)r->y0 - r->ty;
    int64_t clip_right = (int64_t)r->x1 - r->tx, clip_bottom = (int64_t)r->y1 - r->ty;
    if (x >= clip_right || y >= clip_bottom || x + font->width <= clip_left || y + font->height <= clip_top) return;
    int stride = (font->source_width + 7) / 8;
    int row_start = y < clip_top ? (int)(clip_top - y) : 0;
    int row_end = y + font->height > clip_bottom ? (int)(clip_bottom - y) : font->height;
    int col_start = x < clip_left ? (int)(clip_left - x) : 0;
    int col_end = x + font->width > clip_right ? (int)(clip_right - x) : font->width;
    for (int row = row_start; row < row_end; row++) {
        const uint8_t *source = bitmap + rows[row] * stride;
        int run = -1;
        for (int col = col_start; col <= col_end; col++) {
            int sx = col < col_end ? font->columns[col] : 0;
            bool on = col < col_end && (source[sx / 8] & (0x80U >> (sx & 7)));
            if (on && run < 0) run = col;
            if (!on && run >= 0) {
                display_raster_run_unchecked(r, (int)(x + run + r->tx), (int)(y + row + r->ty), col - run, pen);
                run = -1;
            }
        }
    }
}

static esp_err_t walk_text(display_raster_t *r, int x, int y, const char *text, size_t length, const font_view_t *font,
                           const uint8_t *rows, const display_raster_pen_t *pen, int *width, int *height)
{
    int64_t column = 0, row = 0, widest = 0;
    for (size_t offset = 0; offset < length;) {
        uint32_t cp;
        if (!next_codepoint(text, length, &offset, &cp)) return ESP_ERR_INVALID_ARG;
        if (cp == '\n' || cp == '\r') {
            if (column > widest) widest = column;
            column = 0;
            if (cp == '\n') row += font->height;
        } else if (cp == '\t') column += font->width * 4;
        else {
            const uint8_t *bitmap = glyph(font, cp);
            if (!bitmap) return ESP_ERR_NOT_FOUND;
            if (r && pen->color.a) draw_glyph(r, (int64_t)x + column, (int64_t)y + row, bitmap, font, rows, pen);
            column += font->width;
        }
        if (column > INT_MAX || row + font->height > INT_MAX) return ESP_ERR_INVALID_SIZE;
    }
    if (column > widest) widest = column;
    if (width) *width = (int)widest;
    if (height) *height = length ? (int)(row + font->height) : 0;
    return ESP_OK;
}

esp_err_t display_text_measure(const char *text, size_t length, const display_text_options_t *options, int *width, int *height)
{
    if (!text || !options || !width || !height) return ESP_ERR_INVALID_ARG;
    font_view_t font;
    esp_err_t err = font_view(options, &font);
    return err == ESP_OK ? walk_text(NULL, 0, 0, text, length, &font, NULL, NULL, width, height) : err;
}

esp_err_t display_text_draw(display_raster_t *r, int x, int y, const char *text, size_t length, const display_text_options_t *options)
{
    if (!r || !text || !options) return ESP_ERR_INVALID_ARG;
    /* Validate all glyphs before modifying the framebuffer. */
    font_view_t font;
    esp_err_t err = font_view(options, &font);
    display_raster_pen_t pen = display_raster_make_pen(options->color);
    uint8_t rows[FONT_MAX_DIMENSION];
    if (err == ESP_OK) {
        for (int row = 0; row < font.height; ++row) rows[row] = row * font.source_height / font.height;
    }
    int width = 0, height = 0;
    if (err == ESP_OK) err = walk_text(NULL, 0, 0, text, length, &font, NULL, NULL, &width, &height);
    if (err == ESP_OK) err = walk_text(r, x, y, text, length, &font, rows, &pen, NULL, NULL);
    int64_t left = max64((int64_t)x + r->tx, r->x0), top = max64((int64_t)y + r->ty, r->y0);
    int64_t right = min64((int64_t)x + width + r->tx, r->x1), bottom = min64((int64_t)y + height + r->ty, r->y1);
    if (err == ESP_OK && options->color.a && left < right && top < bottom) {
        display_dirty_mark(r->dirty, (int)left, (int)top, (int)(right - left), (int)(bottom - top));
    }
    return err;
}
