/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "system_ui_private.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "claw_skill.h"
#include "esp_check.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"

#define SYSTEM_UI_LAUNCHER_DEFAULT_ROWS 3
#define SYSTEM_UI_LAUNCHER_DEFAULT_COLS 3
#define SYSTEM_UI_LAUNCHER_MAX_ICON_FILE_SIZE (256 * 1024)
#define SYSTEM_UI_LAUNCHER_TITLE_PAD_X 2

static const int32_t s_launcher_min_icon_side = 48;
static const int32_t s_launcher_max_icon_side = 96;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t cell_w;
    int32_t tile_h;
    int32_t icon;
    int32_t cols;
    int32_t gap;
    int32_t title_h;
    int32_t icon_title_gap;
} system_ui_launcher_grid_t;

static size_t system_ui_launcher_tiles_per_page(void)
{
    return (size_t)s_ui.launcher_layout.rows * (size_t)s_ui.launcher_layout.cols;
}

static void system_ui_launcher_set_default_layout(void)
{
    s_ui.launcher_layout.rows = SYSTEM_UI_LAUNCHER_DEFAULT_ROWS;
    s_ui.launcher_layout.cols = SYSTEM_UI_LAUNCHER_DEFAULT_COLS;
}

static void system_ui_launcher_ensure_page_count(void)
{
    size_t tiles_per_page = system_ui_launcher_tiles_per_page();

    if (tiles_per_page > 0) {
        s_ui.launcher_page_count = (s_ui.launcher_app_count + tiles_per_page - 1) / tiles_per_page;
    }
}

static esp_err_t system_ui_launcher_read_binary_file(const char *path, uint8_t **out, size_t *out_size)
{
    struct stat st;
    FILE *file = NULL;
    uint8_t *buf = NULL;
    size_t read_len;

    ESP_RETURN_ON_FALSE(path && out && out_size, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "invalid binary read args");
    *out = NULL;
    *out_size = 0;

    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_FALSE(st.st_size > 0 && (size_t)st.st_size <= SYSTEM_UI_LAUNCHER_MAX_ICON_FILE_SIZE,
                        ESP_ERR_INVALID_SIZE, SYSTEM_UI_TAG, "invalid launcher icon size: %ld", (long)st.st_size);

    buf = malloc((size_t)st.st_size);
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG, "alloc launcher icon failed");

    file = fopen(path, "rb");
    if (!file) {
        free(buf);
        return ESP_FAIL;
    }
    read_len = fread(buf, 1, (size_t)st.st_size, file);
    fclose(file);
    if (read_len != (size_t)st.st_size) {
        free(buf);
        return ESP_FAIL;
    }

    *out = buf;
    *out_size = read_len;
    return ESP_OK;
}

static bool system_ui_launcher_action_is_valid(const char *action)
{
    if (!action || action[0] != '/' || strstr(action, "..")) {
        return false;
    }
    return true;
}

static bool system_ui_launcher_icon_path_is_valid(const char *path)
{
    size_t len;

    if (!path || path[0] != '/' || strstr(path, "..")) {
        return false;
    }
    len = strlen(path);
    return (len > 4 && strcasecmp(path + len - 4, ".jpg") == 0) || (len > 5 && strcasecmp(path + len - 5, ".jpeg") == 0);
}

static void system_ui_launcher_unload_app_icon(system_ui_launcher_app_t *app)
{
    if (!app) {
        return;
    }
    free(app->icon_data);
    app->icon_data = NULL;
    app->icon_data_size = 0;
    app->icon_side = 0;
    memset(&app->icon_dsc, 0, sizeof(app->icon_dsc));
}

static void system_ui_launcher_unload_default_icon(void)
{
    free(s_ui.launcher_default_icon_data);
    s_ui.launcher_default_icon_data = NULL;
    s_ui.launcher_default_icon_data_size = 0;
    memset(&s_ui.launcher_default_icon_dsc, 0, sizeof(s_ui.launcher_default_icon_dsc));
}

static void system_ui_launcher_free_app_list(void)
{
    system_ui_launcher_app_t *app = s_ui.launcher_apps;

    while (app) {
        system_ui_launcher_app_t *next = app->next;
        system_ui_launcher_unload_app_icon(app);
        free(app);
        app = next;
    }
    s_ui.launcher_apps = NULL;
    s_ui.launcher_app_count = 0;
}

static void system_ui_launcher_unload_icons(void)
{
    system_ui_launcher_free_app_list();
    system_ui_launcher_unload_default_icon();
}

static void system_ui_launcher_scale_rgb565_nearest(const uint8_t *src,
                                                     uint16_t src_w,
                                                     uint16_t src_h,
                                                     uint8_t *dst,
                                                     uint16_t dst_w,
                                                     uint16_t dst_h)
{
    const uint16_t *src_pixels = (const uint16_t *)src;
    uint16_t *dst_pixels = (uint16_t *)dst;

    /* Scale after decode so launcher icons can use ordinary JPEG dimensions. */
    for (uint16_t y = 0; y < dst_h; y++) {
        uint32_t src_y = ((uint32_t)y * src_h) / dst_h;
        for (uint16_t x = 0; x < dst_w; x++) {
            uint32_t src_x = ((uint32_t)x * src_w) / dst_w;
            dst_pixels[(size_t)y * dst_w + x] = src_pixels[(size_t)src_y * src_w + src_x];
        }
    }
}

static esp_err_t system_ui_launcher_decode_jpeg_icon(const uint8_t *jpeg_data,
                                                      size_t jpeg_data_size,
                                                      uint16_t target_side,
                                                      uint8_t **out_pixels,
                                                      size_t *out_pixels_size)
{
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_dec_handle_t decoder = NULL;
    jpeg_dec_io_t io = {0};
    jpeg_dec_header_info_t header = {0};
    uint8_t *decoded = NULL;
    uint8_t *scaled = NULL;
    int decoded_len = 0;
    jpeg_error_t jpeg_err;

    ESP_RETURN_ON_FALSE(jpeg_data && jpeg_data_size > 0 && jpeg_data_size <= INT_MAX && target_side > 0 && out_pixels && out_pixels_size,
                        ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "invalid jpeg icon args");
    *out_pixels = NULL;
    *out_pixels_size = 0;

    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_err = jpeg_dec_open(&config, &decoder);
    ESP_RETURN_ON_FALSE(jpeg_err == JPEG_ERR_OK, ESP_FAIL, SYSTEM_UI_TAG, "jpeg icon decoder open failed: %d", jpeg_err);

    io.inbuf = (uint8_t *)jpeg_data;
    io.inbuf_len = (int)jpeg_data_size;
    jpeg_err = jpeg_dec_parse_header(decoder, &io, &header);
    if (jpeg_err != JPEG_ERR_OK || header.width == 0 || header.height == 0) {
        ESP_LOGW(SYSTEM_UI_TAG, "jpeg icon header invalid: err=%d size=%ux%u", jpeg_err, (unsigned)header.width, (unsigned)header.height);
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint32_t)header.width > (uint32_t)INT_MAX / sizeof(uint16_t) / header.height) {
        ESP_LOGW(SYSTEM_UI_TAG, "jpeg icon too large: %ux%u", (unsigned)header.width, (unsigned)header.height);
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }

    decoded_len = (int)header.width * (int)header.height * (int)sizeof(uint16_t);
    decoded = (uint8_t *)jpeg_calloc_align((size_t)decoded_len, 16);
    if (!decoded) {
        ESP_LOGE(SYSTEM_UI_TAG, "alloc decoded jpeg icon failed: %d bytes", decoded_len);
        jpeg_dec_close(decoder);
        return ESP_ERR_NO_MEM;
    }

    io.outbuf = decoded;
    jpeg_err = jpeg_dec_process(decoder, &io);
    jpeg_dec_close(decoder);
    if (jpeg_err != JPEG_ERR_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "jpeg icon decode failed: %d", jpeg_err);
        jpeg_free_align(decoded);
        return ESP_FAIL;
    }

    scaled = calloc(1, (size_t)target_side * target_side * sizeof(uint16_t));
    if (!scaled) {
        ESP_LOGE(SYSTEM_UI_TAG, "alloc scaled launcher icon failed");
        jpeg_free_align(decoded);
        return ESP_ERR_NO_MEM;
    }
    system_ui_launcher_scale_rgb565_nearest(decoded, header.width, header.height, scaled, target_side, target_side);
    jpeg_free_align(decoded);

    *out_pixels = scaled;
    *out_pixels_size = (size_t)target_side * target_side * sizeof(uint16_t);
    return ESP_OK;
}

static esp_err_t system_ui_launcher_load_icon_file(const char *path,
                                                    uint16_t target_side,
                                                    lv_image_dsc_t *out_dsc,
                                                    uint8_t **out_data,
                                                    size_t *out_data_size)
{
    uint8_t *file_data = NULL;
    size_t file_data_size = 0;
    uint8_t *pixels = NULL;
    size_t pixels_size = 0;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(path && target_side > 0 && out_dsc && out_data && out_data_size,
                        ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "invalid icon file args");
    ret = system_ui_launcher_read_binary_file(path, &file_data, &file_data_size);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = system_ui_launcher_decode_jpeg_icon(file_data, file_data_size, target_side, &pixels, &pixels_size);
    free(file_data);
    if (ret != ESP_OK) {
        return ret;
    }

    memset(out_dsc, 0, sizeof(*out_dsc));
    out_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    out_dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    out_dsc->header.flags = 0;
    out_dsc->header.w = target_side;
    out_dsc->header.h = target_side;
    out_dsc->header.stride = target_side * sizeof(uint16_t);
    out_dsc->data_size = pixels_size;
    out_dsc->data = pixels;
    *out_data = pixels;
    *out_data_size = pixels_size;
    return ESP_OK;
}

static esp_err_t system_ui_launcher_load_app_icon(system_ui_launcher_app_t *app, int32_t target_side)
{
    lv_image_dsc_t icon = {0};
    uint8_t *data = NULL;
    size_t data_size = 0;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(app && target_side > 0 && target_side <= UINT16_MAX, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "invalid launcher app icon args");
    if (!app->icon_path[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    if (app->icon_data && app->icon_side == (uint16_t)target_side) {
        return ESP_OK;
    }
    ret = system_ui_launcher_load_icon_file(app->icon_path, (uint16_t)target_side, &icon, &data, &data_size);
    if (ret != ESP_OK) {
        return ret;
    }
    system_ui_launcher_unload_app_icon(app);
    app->icon_dsc = icon;
    app->icon_data = data;
    app->icon_data_size = data_size;
    app->icon_side = (uint16_t)target_side;
    return ESP_OK;
}

static int system_ui_launcher_compare_app(const system_ui_launcher_app_t *a, const system_ui_launcher_app_t *b)
{
    if (a->order != b->order) {
        return a->order - b->order;
    }
    return strcmp(a->title, b->title);
}

static void system_ui_launcher_insert_app_sorted(system_ui_launcher_app_t *app)
{
    system_ui_launcher_app_t **cursor = &s_ui.launcher_apps;

    while (*cursor && system_ui_launcher_compare_app(*cursor, app) <= 0) {
        cursor = &(*cursor)->next;
    }
    app->next = *cursor;
    *cursor = app;
    s_ui.launcher_app_count++;
}

static esp_err_t system_ui_launcher_add_skill_entry(const claw_skill_catalog_entry_t *entry, void *user_ctx)
{
    system_ui_launcher_app_t *app = NULL;

    (void)user_ctx;

    if (!entry || !entry->execution || !entry->execution->visible) {
        return ESP_OK;
    }
    if (s_ui.launcher_app_count >= SYSTEM_UI_LAUNCHER_MAX_APPS) {
        ESP_LOGW(SYSTEM_UI_TAG, "skip launcher app: capacity reached");
        return ESP_OK;
    }
    if (!entry->id || !entry->id[0] || !entry->execution->entry || !system_ui_launcher_action_is_valid(entry->execution->entry)) {
        ESP_LOGW(SYSTEM_UI_TAG, "skip invalid launcher execution: id=%s", entry && entry->id ? entry->id : "(null)");
        return ESP_OK;
    }
    if (entry->execution->icon && !system_ui_launcher_icon_path_is_valid(entry->execution->icon)) {
        ESP_LOGW(SYSTEM_UI_TAG, "ignore invalid launcher icon: id=%s", entry->id);
    }

    app = calloc(1, sizeof(*app));
    ESP_RETURN_ON_FALSE(app != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG, "alloc launcher app failed");

    if (strlcpy(app->id, entry->id, sizeof(app->id)) >= sizeof(app->id) ||
            strlcpy(app->title, entry->id, sizeof(app->title)) >= sizeof(app->title) ||
            strlcpy(app->action, entry->execution->entry, sizeof(app->action)) >= sizeof(app->action)) {
        ESP_LOGW(SYSTEM_UI_TAG, "skip launcher app with long fields: id=%s", entry->id);
        free(app);
        return ESP_OK;
    }
    app->order = entry->execution->order;
    if (entry->execution->args_json && strlcpy(app->args_json, entry->execution->args_json, sizeof(app->args_json)) >= sizeof(app->args_json)) {
        ESP_LOGW(SYSTEM_UI_TAG, "skip launcher app with long args: id=%s", entry->id);
        free(app);
        return ESP_OK;
    }
    if (entry->execution->icon && system_ui_launcher_icon_path_is_valid(entry->execution->icon)) {
        if (strlcpy(app->icon_path, entry->execution->icon, sizeof(app->icon_path)) >= sizeof(app->icon_path)) {
            ESP_LOGW(SYSTEM_UI_TAG, "ignore long launcher icon path: id=%s", entry->id);
            app->icon_path[0] = '\0';
        }
    }

    system_ui_launcher_insert_app_sorted(app);
    return ESP_OK;
}

esp_err_t system_ui_launcher_load_locked(void)
{
    esp_err_t ret;

    system_ui_launcher_unload_icons();
    s_ui.launcher_page_count = 0;
    system_ui_launcher_set_default_layout();

    ret = claw_skill_foreach_catalog_entry(system_ui_launcher_add_skill_entry, NULL);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(SYSTEM_UI_TAG, "launcher deferred: skill registry not ready");
        system_ui_launcher_ensure_page_count();
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, SYSTEM_UI_TAG, "load launcher skills failed");

    system_ui_launcher_ensure_page_count();
    ESP_LOGI(SYSTEM_UI_TAG, "loaded %u launcher apps in %u pages",
             (unsigned)s_ui.launcher_app_count, (unsigned)s_ui.launcher_page_count);
    return ESP_OK;
}

static system_ui_launcher_grid_t system_ui_launcher_compute_grid(int32_t width, int32_t height)
{
    int32_t short_side = system_ui_min_i32(width, height);
    int32_t margin = system_ui_clamp_i32(short_side / 15, 14, 30);
    int32_t gap = system_ui_clamp_i32(short_side / 22, 10, 20);
    int32_t header_h = system_ui_clamp_i32(short_side / 7, 34, 58);
    int32_t page_h = system_ui_clamp_i32(short_side / 8, 24, 44);
    int32_t title_h = system_ui_clamp_i32(short_side / 8, 28, 42);
    int32_t icon_title_gap = system_ui_clamp_i32(short_side / 48, 5, 9);
    int32_t grid_top = header_h + margin / 2;
    int32_t page_top = height - page_h - margin / 2;
    int32_t grid_h = system_ui_max_i32(80, page_top - grid_top);
    int32_t grid_w = system_ui_max_i32(120, width - margin * 2);
    int32_t rows = s_ui.launcher_layout.rows;
    int32_t cols = s_ui.launcher_layout.cols;
    int32_t cell_w = system_ui_max_i32(42, (grid_w - gap * (cols - 1)) / cols);
    int32_t max_icon_h = (grid_h - gap * (rows - 1)) / rows - title_h - icon_title_gap;
    int32_t max_icon = system_ui_min_i32(cell_w, max_icon_h);
    int32_t icon = system_ui_clamp_i32(max_icon, s_launcher_min_icon_side, s_launcher_max_icon_side);

    int32_t tile_h = icon + icon_title_gap + title_h;
    int32_t used_h = tile_h * rows + gap * (rows - 1);
    int32_t grid_y = grid_top + (grid_h - used_h) / 2;
    if (grid_y < grid_top) {
        grid_y = grid_top;
    }

    return (system_ui_launcher_grid_t) {
        .x = margin,
        .y = grid_y,
        .cell_w = cell_w,
        .tile_h = tile_h,
        .icon = icon,
        .cols = cols,
        .gap = gap,
        .title_h = title_h,
        .icon_title_gap = icon_title_gap,
    };
}

static void system_ui_launcher_app_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    system_ui_launcher_app_t *app = (system_ui_launcher_app_t *)lv_event_get_user_data(event);
    if (!app) {
        return;
    }

    system_ui_work_event_t work_event = {
        .type = SYSTEM_UI_WORK_EVENT_LAUNCHER_SELECT,
        .generation = s_ui.generation,
    };
    strlcpy(work_event.launcher_id, app->id, sizeof(work_event.launcher_id));
    strlcpy(work_event.launcher_title, app->title, sizeof(work_event.launcher_title));
    strlcpy(work_event.launcher_action, app->action, sizeof(work_event.launcher_action));
    strlcpy(work_event.launcher_args_json, app->args_json, sizeof(work_event.launcher_args_json));
    (void)system_ui_post_work_event(&work_event, 0);
}

static esp_err_t system_ui_launcher_create_app_card(lv_obj_t *tile,
                                                     const system_ui_launcher_grid_t *grid,
                                                     size_t tile_index,
                                                     system_ui_launcher_app_t *app)
{
    int32_t col = (int32_t)(tile_index % (size_t)grid->cols);
    int32_t row = (int32_t)(tile_index / (size_t)grid->cols);
    int32_t cell_x = grid->x + col * (grid->cell_w + grid->gap);
    int32_t cell_y = grid->y + row * (grid->tile_h + grid->gap);
    const lv_image_dsc_t *icon = NULL;

    lv_obj_t *card = lv_obj_create(tile);
    ESP_RETURN_ON_FALSE(card != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG, "create launcher card failed");
    lv_obj_set_pos(card, cell_x, cell_y);
    lv_obj_set_size(card, grid->cell_w, grid->tile_h);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Decode lazily with the exact side selected by the current parent grid. */
    if (app->icon_path[0]) {
        esp_err_t ret = system_ui_launcher_load_app_icon(app, grid->icon);
        if (ret != ESP_OK) {
            ESP_LOGW(SYSTEM_UI_TAG, "launcher icon load failed: title=%s size=%ld err=%s", app->title, (long)grid->icon, esp_err_to_name(ret));
        }
    }
    if (app->icon_data) {
        icon = &app->icon_dsc;
    } else if (s_ui.launcher_default_icon_data) {
        icon = &s_ui.launcher_default_icon_dsc;
    }
    if (icon) {
        lv_obj_t *image = lv_image_create(card);
        ESP_RETURN_ON_FALSE(image != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG, "create launcher image failed");
        lv_image_set_src(image, icon);
        if (icon->header.w > 0 && icon->header.h > 0) {
            uint32_t max_dim = icon->header.w > icon->header.h ? icon->header.w : icon->header.h;
            if ((int32_t)max_dim != grid->icon) {
                lv_image_set_scale(image, (uint32_t)(grid->icon * LV_SCALE_NONE / max_dim));
            }
        }
        lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(image, LV_OBJ_FLAG_CLICKABLE);
        system_ui_add_click_feedback(image);
        lv_obj_add_event_cb(image, system_ui_launcher_app_event_cb, LV_EVENT_CLICKED, app);
    } else {
        lv_obj_t *fallback = lv_obj_create(card);
        ESP_RETURN_ON_FALSE(fallback != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG, "create launcher fallback failed");
        lv_obj_set_size(fallback, grid->icon, grid->icon);
        lv_obj_align(fallback, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(fallback, 0, 0);
        lv_obj_set_style_bg_color(fallback, system_ui_color(SYSTEM_UI_COLOR_PANEL), 0);
        lv_obj_set_style_bg_opa(fallback, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(fallback, system_ui_color(SYSTEM_UI_COLOR_LINE), 0);
        lv_obj_set_style_border_width(fallback, 1, 0);
        lv_obj_set_style_pad_all(fallback, 0, 0);
        lv_obj_clear_flag(fallback, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(fallback, LV_OBJ_FLAG_CLICKABLE);
        system_ui_add_click_feedback(fallback);
        lv_obj_add_event_cb(fallback, system_ui_launcher_app_event_cb, LV_EVENT_CLICKED, app);

        lv_obj_t *icon_label = lv_label_create(fallback);
        ESP_RETURN_ON_FALSE(icon_label != NULL, ESP_ERR_NO_MEM,
                            SYSTEM_UI_TAG, "create launcher icon label failed");
        lv_label_set_text(icon_label, "APP");
        lv_obj_set_style_text_color(icon_label, system_ui_color(SYSTEM_UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);
        system_ui_apply_font(icon_label);
        lv_obj_remove_flag(icon_label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(icon_label);

        for (int32_t i = 0; i < 3; i++) {
            lv_obj_t *bar = lv_obj_create(fallback);
            ESP_RETURN_ON_FALSE(bar != NULL, ESP_ERR_NO_MEM,
                                SYSTEM_UI_TAG, "create launcher icon bar failed");
            lv_obj_set_size(bar, 4, 8 + i * 5);
            lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 8 + i * 10, -7);
            lv_obj_set_style_radius(bar, 0, 0);
            lv_obj_set_style_bg_color(bar, system_ui_color(SYSTEM_UI_COLOR_TEXT), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    lv_obj_t *label = lv_label_create(card);
    ESP_RETURN_ON_FALSE(label != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG, "create launcher label failed");
    lv_label_set_text(label, app->title);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(label,
                    grid->cell_w - SYSTEM_UI_LAUNCHER_TITLE_PAD_X * 2,
                    grid->title_h);
    lv_obj_set_style_text_color(label, system_ui_color(SYSTEM_UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    system_ui_apply_font(label);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, grid->icon + grid->icon_title_gap);
    return ESP_OK;
}

static system_ui_launcher_app_t *system_ui_launcher_app_at(size_t index)
{
    system_ui_launcher_app_t *app = s_ui.launcher_apps;

    while (app && index > 0) {
        app = app->next;
        index--;
    }
    return app;
}

static esp_err_t system_ui_launcher_create_page_locked(size_t page_index)
{
    lv_dir_t dir = LV_DIR_LEFT;
    if (page_index + 1 < s_ui.launcher_page_count) {
        dir |= LV_DIR_RIGHT;
    }

    /* Launcher pages share the same row as the clock page and start at column 1. */
    lv_obj_t *tile = lv_tileview_add_tile(s_ui.tileview, (uint8_t)(page_index + 1), 0, dir);
    ESP_RETURN_ON_FALSE(tile != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG, "create launcher tile failed");
    if (page_index == 0) {
        s_ui.launcher_first_tile = tile;
    }
    lv_obj_set_style_bg_color(tile, system_ui_color(SYSTEM_UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    system_ui_apply_font(tile);

    int32_t short_side = system_ui_short_side_from(s_ui.width, s_ui.height);
    int32_t header_pad = system_ui_clamp_i32(short_side / 16, 14, 30);
    int32_t header_y = system_ui_clamp_i32(short_side / 22, 10, 20);

    char header[48];
    snprintf(header, sizeof(header), "%u/%u",
             (unsigned)(page_index + 1), (unsigned)s_ui.launcher_page_count);
    lv_obj_t *header_label = lv_label_create(tile);
    ESP_RETURN_ON_FALSE(header_label != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG, "create launcher header failed");
    lv_label_set_text(header_label, header);
    lv_obj_set_style_text_color(header_label, system_ui_color(SYSTEM_UI_COLOR_MUTED), 0);
    system_ui_apply_font(header_label);
    lv_obj_align(header_label, LV_ALIGN_TOP_RIGHT, -header_pad, header_y);

    system_ui_launcher_grid_t grid =
        system_ui_launcher_compute_grid((int32_t)s_ui.width, (int32_t)s_ui.height);
    size_t tiles_per_page = system_ui_launcher_tiles_per_page();
    size_t start = page_index * tiles_per_page;
    system_ui_launcher_app_t *app = system_ui_launcher_app_at(start);
    for (size_t i = 0; app && i < tiles_per_page; i++, app = app->next) {
        ESP_RETURN_ON_ERROR(system_ui_launcher_create_app_card(tile, &grid, i, app), SYSTEM_UI_TAG, "create launcher app card failed");
    }
    return ESP_OK;
}

esp_err_t system_ui_launcher_create_pages_locked(void)
{
    for (size_t i = 0; i < s_ui.launcher_page_count; i++) {
        ESP_RETURN_ON_ERROR(system_ui_launcher_create_page_locked(i),
                            SYSTEM_UI_TAG, "create launcher page failed");
    }
    return ESP_OK;
}

void system_ui_launcher_delete_locked(void)
{
    system_ui_launcher_unload_icons();
    s_ui.launcher_page_count = 0;
    s_ui.launcher_first_tile = NULL;
}

esp_err_t system_ui_launcher_set_select_callback(system_ui_launcher_select_cb_t cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(s_ui.started, ESP_ERR_INVALID_STATE, SYSTEM_UI_TAG, "runtime not started");
    ESP_RETURN_ON_ERROR(system_ui_callback_lock(), SYSTEM_UI_TAG, "callback lock failed");
    s_ui.launcher_select_cb = cb;
    s_ui.launcher_select_user_ctx = user_ctx;
    system_ui_callback_unlock();
    return ESP_OK;
}
