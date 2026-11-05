/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_text.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_painter_font.h"

#define FONT_HEADER_SIZE 20
#define FONT_RECORD_SIZE 18
#define FONT_MAX_DIMENSION 64
#define FONT_MIN_SIZE 8
#define FONT_MAX_GLYPHS 4096
#define FONT_MAX_BYTES (1024 * 1024)
#define FONT_NO_FALLBACK UINT32_MAX

static const char *TAG = "display_text";

struct display_font_t {
    int16_t ascent, descent, default_advance;
    uint32_t count, fallback;
    size_t bitmap_size;
    uint8_t data[];
};

typedef struct {
    const esp_painter_basic_font_t *builtin;
    display_font_handle_t custom;
    int width, height;
    int source_width, source_height;
    int tab_advance;
    uint8_t columns[FONT_MAX_DIMENSION];
} font_view_t;

typedef struct {
    const uint8_t *bitmap;
    int source_width, source_height;
    int width, height;
    int x_offset, top_offset, advance;
} glyph_view_t;

typedef struct {
    int width, height;
    int64_t left, top, right, bottom;
    bool has_ink;
} text_bounds_t;

static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }

static uint16_t read16(const uint8_t *p)
{
    return p[0] | ((uint16_t)p[1] << 8);
}

static int16_t read_s16(const uint8_t *p)
{
    return (int16_t)read16(p);
}

static uint32_t read32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool valid_codepoint(uint32_t cp)
{
    return cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
}

static const uint8_t *find_record(display_font_handle_t font, uint32_t cp)
{
    uint32_t lo = 0, hi = font->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *record = font->data + mid * FONT_RECORD_SIZE;
        uint32_t code = read32(record);
        if (code == cp) return record;
        if (code < cp) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

esp_err_t display_font_create(const char *path, display_font_handle_t *ret_font)
{
    if (!path || !ret_font) return ESP_ERR_INVALID_ARG;
    *ret_font = NULL;
    FILE *file = fopen(path, "rb");
    if (!file) { ESP_LOGE(TAG, "font open failed: %s", path); return ESP_ERR_NOT_FOUND; }

    esp_err_t err = ESP_ERR_INVALID_SIZE;
    display_font_handle_t font = NULL;
    uint8_t header[FONT_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "DFN1", 4)) goto done;

    int ascent = read_s16(header + 4), descent = read_s16(header + 6), default_advance = read_s16(header + 8);
    uint32_t count = read32(header + 12), fallback = read32(header + 16);
    if (read16(header + 10) != 0 || ascent < 0 || descent < 0 || ascent + descent < 1 || ascent + descent > FONT_MAX_DIMENSION ||
        default_advance < 1 || default_advance > FONT_MAX_DIMENSION || !count || count > FONT_MAX_GLYPHS ||
        (fallback != FONT_NO_FALLBACK && !valid_codepoint(fallback))) goto done;

    if (fseek(file, 0, SEEK_END) != 0) goto done;
    long file_size = ftell(file);
    size_t index_size = (size_t)count * FONT_RECORD_SIZE;
    if (file_size < FONT_HEADER_SIZE || file_size > FONT_MAX_BYTES || (size_t)file_size - FONT_HEADER_SIZE < index_size) goto done;
    size_t data_size = (size_t)file_size - FONT_HEADER_SIZE;
    if (fseek(file, FONT_HEADER_SIZE, SEEK_SET) != 0) goto done;

    font = malloc(sizeof(*font) + data_size);
    if (!font) { err = ESP_ERR_NO_MEM; goto done; }
    font->ascent = ascent;
    font->descent = descent;
    font->default_advance = default_advance;
    font->count = count;
    font->fallback = fallback;
    font->bitmap_size = data_size - index_size;
    if (fread(font->data, 1, data_size, file) != data_size || fgetc(file) != EOF || ferror(file)) goto done;

    size_t bitmap_offset = 0;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *record = font->data + i * FONT_RECORD_SIZE;
        uint32_t cp = read32(record), offset = read32(record + 4);
        int advance = read_s16(record + 8), x_offset = read_s16(record + 10), y_offset = read_s16(record + 12);
        unsigned width = read16(record + 14), height = read16(record + 16);
        size_t glyph_size = ((width + 7) / 8) * height;
        bool valid_box = ((!width && !height) || (width && height && width <= FONT_MAX_DIMENSION && height <= FONT_MAX_DIMENSION)) &&
                         x_offset >= -FONT_MAX_DIMENSION && x_offset <= FONT_MAX_DIMENSION &&
                         y_offset >= -descent && y_offset + (int)height <= ascent;
        if (!valid_codepoint(cp) || (i && cp <= previous) || offset != bitmap_offset || advance < 0 || advance > FONT_MAX_DIMENSION ||
            !valid_box || bitmap_offset > font->bitmap_size || glyph_size > font->bitmap_size - bitmap_offset) {
            ESP_LOGE(TAG, "invalid glyph record: %" PRIu32, i);
            goto done;
        }
        bitmap_offset += glyph_size;
        previous = cp;
    }
    if (bitmap_offset != font->bitmap_size || (fallback != FONT_NO_FALLBACK && !find_record(font, fallback))) goto done;

    err = ESP_OK;
done:
    fclose(file);
    if (err != ESP_OK) {
        free(font);
        ESP_LOGE(TAG, "font load failed: %s", esp_err_to_name(err));
    } else {
        *ret_font = font;
    }
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

static void decode_custom_glyph(display_font_handle_t font, const uint8_t *record, glyph_view_t *glyph)
{
    size_t index_size = (size_t)font->count * FONT_RECORD_SIZE;
    unsigned width = read16(record + 14), height = read16(record + 16);
    glyph->bitmap = font->data + index_size + read32(record + 4);
    glyph->source_width = glyph->width = width;
    glyph->source_height = glyph->height = height;
    glyph->x_offset = read_s16(record + 10);
    glyph->top_offset = font->ascent - (read_s16(record + 12) + (int)height);
    glyph->advance = read_s16(record + 8);
}

static esp_err_t font_view(const display_text_options_t *options, font_view_t *view)
{
    memset(view, 0, sizeof(*view));
    view->custom = options->font;
    view->builtin = options->font ? NULL : builtin_font();
    if (!view->custom && !view->builtin) return ESP_ERR_NOT_SUPPORTED;
    if (view->custom) {
        view->height = view->custom->ascent + view->custom->descent;
        view->width = view->custom->default_advance;
        const uint8_t *space = find_record(view->custom, ' ');
        view->tab_advance = (space ? read_s16(space + 8) : view->width) * 4;
        return ESP_OK;
    }

    if (options->font_size < FONT_MIN_SIZE || options->font_size > FONT_MAX_DIMENSION) return ESP_ERR_INVALID_ARG;
    view->source_width = view->builtin->width;
    view->source_height = view->builtin->height;
    view->height = options->font_size;
    view->width = (view->source_width * view->height + view->source_height / 2) / view->source_height;
    if (view->width < 1) view->width = 1;
    if (view->width > FONT_MAX_DIMENSION) return ESP_ERR_INVALID_SIZE;
    view->tab_advance = view->width * 4;
    for (int x = 0; x < view->width; x++) view->columns[x] = x * view->source_width / view->width;
    return ESP_OK;
}

static bool glyph_view(const font_view_t *font, uint32_t cp, glyph_view_t *glyph)
{
    if (font->builtin) {
        if (cp < 32 || cp > 126) return false;
        glyph->bitmap = font->builtin->bitmap + (cp - 32) * ((font->source_width + 7) / 8) * font->source_height;
        glyph->source_width = font->source_width;
        glyph->source_height = font->source_height;
        glyph->width = font->width;
        glyph->height = font->height;
        glyph->x_offset = glyph->top_offset = 0;
        glyph->advance = font->width;
        return true;
    }

    const uint8_t *record = find_record(font->custom, cp);
    if (!record && font->custom->fallback != FONT_NO_FALLBACK) record = find_record(font->custom, font->custom->fallback);
    if (!record) return false;
    decode_custom_glyph(font->custom, record, glyph);
    return true;
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

static void draw_glyph(display_raster_t *r, int64_t x, int64_t y, const glyph_view_t *glyph, const font_view_t *font,
                       const uint8_t *rows, const display_raster_pen_t *pen)
{
    if (!glyph->width || !glyph->height) return;
    int64_t clip_left = (int64_t)r->x0 - r->tx, clip_top = (int64_t)r->y0 - r->ty;
    int64_t clip_right = (int64_t)r->x1 - r->tx, clip_bottom = (int64_t)r->y1 - r->ty;
    if (x >= clip_right || y >= clip_bottom || x + glyph->width <= clip_left || y + glyph->height <= clip_top) return;
    int stride = (glyph->source_width + 7) / 8;
    int row_start = y < clip_top ? (int)(clip_top - y) : 0;
    int row_end = y + glyph->height > clip_bottom ? (int)(clip_bottom - y) : glyph->height;
    int col_start = x < clip_left ? (int)(clip_left - x) : 0;
    int col_end = x + glyph->width > clip_right ? (int)(clip_right - x) : glyph->width;
    for (int row = row_start; row < row_end; row++) {
        int source_row = font->builtin ? rows[row] : row;
        const uint8_t *source = glyph->bitmap + source_row * stride;
        int run = -1;
        for (int col = col_start; col <= col_end; col++) {
            int sx = col < col_end ? (font->builtin ? font->columns[col] : col) : 0;
            bool on = col < col_end && (source[sx / 8] & (0x80U >> (sx & 7)));
            if (on && run < 0) run = col;
            if (!on && run >= 0) {
                display_raster_run_unchecked(r, (int)(x + run + r->tx), (int)(y + row + r->ty), col - run, pen);
                run = -1;
            }
        }
    }
}

static void add_ink_bounds(text_bounds_t *bounds, int64_t left, int64_t top, int width, int height)
{
    if (!width || !height) return;
    int64_t right = left + width, bottom = top + height;
    if (!bounds->has_ink) {
        bounds->left = left;
        bounds->top = top;
        bounds->right = right;
        bounds->bottom = bottom;
        bounds->has_ink = true;
        return;
    }
    bounds->left = min64(bounds->left, left);
    bounds->top = min64(bounds->top, top);
    bounds->right = max64(bounds->right, right);
    bounds->bottom = max64(bounds->bottom, bottom);
}

static esp_err_t walk_text(display_raster_t *r, int x, int y, const char *text, size_t length, const font_view_t *font,
                           const uint8_t *rows, const display_raster_pen_t *pen, text_bounds_t *bounds)
{
    int64_t column = 0, row = 0, widest = 0;
    for (size_t offset = 0; offset < length;) {
        uint32_t cp;
        if (!next_codepoint(text, length, &offset, &cp)) return ESP_ERR_INVALID_ARG;
        if (cp == '\n' || cp == '\r') {
            if (column > widest) widest = column;
            column = 0;
            if (cp == '\n') row += font->height;
        } else if (cp == '\t') {
            column += font->tab_advance;
        } else {
            glyph_view_t glyph;
            if (!glyph_view(font, cp, &glyph)) return ESP_ERR_NOT_FOUND;
            int64_t glyph_x = column + glyph.x_offset, glyph_y = row + glyph.top_offset;
            if (bounds) add_ink_bounds(bounds, glyph_x, glyph_y, glyph.width, glyph.height);
            if (r && pen->color.a) draw_glyph(r, (int64_t)x + glyph_x, (int64_t)y + glyph_y, &glyph, font, rows, pen);
            column += glyph.advance;
        }
        if (column > INT_MAX || row + font->height > INT_MAX) return ESP_ERR_INVALID_SIZE;
    }
    if (column > widest) widest = column;
    if (bounds) {
        bounds->width = (int)widest;
        bounds->height = length ? (int)(row + font->height) : 0;
    }
    return ESP_OK;
}

esp_err_t display_text_measure(const char *text, size_t length, const display_text_options_t *options, int *width, int *height)
{
    if (!text || !options || !width || !height) return ESP_ERR_INVALID_ARG;
    font_view_t font;
    esp_err_t err = font_view(options, &font);
    text_bounds_t bounds = {0};
    if (err == ESP_OK) err = walk_text(NULL, 0, 0, text, length, &font, NULL, NULL, &bounds);
    if (err == ESP_OK) {
        *width = bounds.width;
        *height = bounds.height;
    }
    return err;
}

esp_err_t display_text_draw(display_raster_t *r, int x, int y, const char *text, size_t length, const display_text_options_t *options)
{
    if (!r || !text || !options) return ESP_ERR_INVALID_ARG;
    /* Validate all glyphs before modifying the framebuffer. */
    font_view_t font;
    esp_err_t err = font_view(options, &font);
    display_raster_pen_t pen = display_raster_make_pen(options->color);
    uint8_t rows[FONT_MAX_DIMENSION];
    if (err == ESP_OK && font.builtin) {
        for (int row = 0; row < font.height; row++) rows[row] = row * font.source_height / font.height;
    }
    text_bounds_t bounds = {0};
    if (err == ESP_OK) err = walk_text(NULL, 0, 0, text, length, &font, NULL, NULL, &bounds);
    if (err == ESP_OK) err = walk_text(r, x, y, text, length, &font, rows, &pen, NULL);
    if (err == ESP_OK && options->color.a && bounds.has_ink) {
        int64_t left = max64((int64_t)x + bounds.left + r->tx, r->x0);
        int64_t top = max64((int64_t)y + bounds.top + r->ty, r->y0);
        int64_t right = min64((int64_t)x + bounds.right + r->tx, r->x1);
        int64_t bottom = min64((int64_t)y + bounds.bottom + r->ty, r->y1);
        if (left < right && top < bottom) display_dirty_mark(r->dirty, (int)left, (int)top, (int)(right - left), (int)(bottom - top));
    }
    return err;
}
