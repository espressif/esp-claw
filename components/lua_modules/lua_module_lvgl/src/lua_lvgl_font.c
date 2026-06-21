/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

#if LV_USE_TINY_TTF

static const char *TAG = "lua_lvgl_font";

static bool lua_lvgl_path_has_parent_ref(const char *path)
{
    const char *p = path;

    while (p && *p) {
        if ((p[0] == '.' && p[1] == '.' && (p[2] == '\0' || p[2] == '/' || p[2] == '\\')) ||
                ((p[0] == '/' || p[0] == '\\') && p[1] == '.' && p[2] == '.' &&
                 (p[3] == '\0' || p[3] == '/' || p[3] == '\\'))) {
            return true;
        }
        p++;
    }
    return false;
}

static const char *lua_lvgl_skip_drive_prefix(const char *path)
{
    if (!path) {
        return NULL;
    }
    if (path[0] == LUA_MODULE_LVGL_FS_LETTER && path[1] == ':') {
        path += 2;
    }
    while (*path == '/' || *path == '\\') {
        path++;
    }
    return path;
}

static bool lua_lvgl_path_starts_with_data_root(const char *path, const char **out_rel)
{
    size_t root_len;

    if (!path || !s_lvgl.data_root[0]) {
        return false;
    }
    root_len = strlen(s_lvgl.data_root);
    if (strncmp(path, s_lvgl.data_root, root_len) != 0) {
        return false;
    }
    if (path[root_len] == '\0') {
        *out_rel = "";
        return true;
    }
    if (path[root_len] == '/' || path[root_len] == '\\') {
        *out_rel = path + root_len + 1;
        return true;
    }
    return false;
}

static esp_err_t lua_lvgl_to_lv_fs_path(const char *path, char *out, size_t out_size)
{
    const char *rel = NULL;
    int written;

    ESP_RETURN_ON_FALSE(path && path[0], ESP_ERR_INVALID_ARG, TAG, "font path is empty");
    ESP_RETURN_ON_FALSE(out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "path output missing");

    if (path[0] == LUA_MODULE_LVGL_FS_LETTER && path[1] == ':') {
        rel = lua_lvgl_skip_drive_prefix(path);
    } else if (lua_lvgl_path_starts_with_data_root(path, &rel)) {
        rel = lua_lvgl_skip_drive_prefix(rel);
    } else if (path[0] == '/' || path[0] == '\\') {
        return ESP_ERR_INVALID_ARG;
    } else {
        rel = lua_lvgl_skip_drive_prefix(path);
    }

    ESP_RETURN_ON_FALSE(rel && rel[0], ESP_ERR_INVALID_ARG, TAG, "font path points to data root");
    ESP_RETURN_ON_FALSE(!lua_lvgl_path_has_parent_ref(rel), ESP_ERR_INVALID_ARG, TAG, "font path escapes data root");

    written = snprintf(out, out_size, "%c:/%s", LUA_MODULE_LVGL_FS_LETTER, rel);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < out_size, ESP_ERR_INVALID_SIZE, TAG, "font path too long");
    return ESP_OK;
}

static esp_err_t lua_lvgl_to_vfs_path(const char *path, char *out, size_t out_size)
{
    const char *rel = lua_lvgl_skip_drive_prefix(path);
    int written;

    ESP_RETURN_ON_FALSE(s_lvgl.data_root[0], ESP_ERR_INVALID_STATE, TAG, "data root is not configured");
    ESP_RETURN_ON_FALSE(rel && rel[0], ESP_ERR_INVALID_ARG, TAG, "fs path is empty");
    ESP_RETURN_ON_FALSE(!lua_lvgl_path_has_parent_ref(rel), ESP_ERR_INVALID_ARG, TAG, "fs path escapes data root");

    written = snprintf(out, out_size, "%s/%s", s_lvgl.data_root, rel);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < out_size, ESP_ERR_INVALID_SIZE, TAG, "vfs path too long");
    return ESP_OK;
}

static bool lua_lvgl_fs_ready_cb(lv_fs_drv_t *drv)
{
    (void)drv;
    return s_lvgl.data_root[0] != '\0';
}

static void *lua_lvgl_fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    char vfs_path[LUA_MODULE_LVGL_PATH_MAX];
    const char *flags = NULL;

    (void)drv;
    if (lua_lvgl_to_vfs_path(path, vfs_path, sizeof(vfs_path)) != ESP_OK) {
        return NULL;
    }
    if (mode == LV_FS_MODE_RD) {
        flags = "rb";
    } else if (mode == LV_FS_MODE_WR) {
        flags = "wb";
    } else if (mode == (LV_FS_MODE_RD | LV_FS_MODE_WR)) {
        flags = "rb+";
    } else {
        return NULL;
    }
    return fopen(vfs_path, flags);
}

static lv_fs_res_t lua_lvgl_fs_close_cb(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    return fclose((FILE *)file_p) == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t lua_lvgl_fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    size_t read_len;

    (void)drv;
    read_len = fread(buf, 1, btr, (FILE *)file_p);
    if (br) {
        *br = (uint32_t)read_len;
    }
    if (read_len < btr && ferror((FILE *)file_p)) {
        return LV_FS_RES_FS_ERR;
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t lua_lvgl_fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
    int origin;

    (void)drv;
    switch (whence) {
    case LV_FS_SEEK_SET:
        origin = SEEK_SET;
        break;
    case LV_FS_SEEK_CUR:
        origin = SEEK_CUR;
        break;
    case LV_FS_SEEK_END:
        origin = SEEK_END;
        break;
    default:
        return LV_FS_RES_INV_PARAM;
    }
    return fseek((FILE *)file_p, (long)pos, origin) == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t lua_lvgl_fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    long pos;

    (void)drv;
    pos = ftell((FILE *)file_p);
    if (pos < 0) {
        return LV_FS_RES_FS_ERR;
    }
    if (pos_p) {
        *pos_p = (uint32_t)pos;
    }
    return LV_FS_RES_OK;
}

esp_err_t lua_lvgl_set_data_root(const char *data_root)
{
    int written;

    if (!data_root || !data_root[0]) {
        s_lvgl.data_root[0] = '\0';
        return ESP_OK;
    }
    written = snprintf(s_lvgl.data_root, sizeof(s_lvgl.data_root), "%s", data_root);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < sizeof(s_lvgl.data_root),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "data root too long");
    while (strlen(s_lvgl.data_root) > 1 && s_lvgl.data_root[strlen(s_lvgl.data_root) - 1] == '/') {
        s_lvgl.data_root[strlen(s_lvgl.data_root) - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t lua_lvgl_register_fs_locked(void)
{
    lv_fs_drv_t *existing;

    if (s_lvgl.fs_registered) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(s_lvgl.data_root[0], ESP_ERR_INVALID_STATE, TAG, "data root is not configured");

    existing = lv_fs_get_drv(LUA_MODULE_LVGL_FS_LETTER);
    ESP_RETURN_ON_FALSE(existing == NULL, ESP_ERR_INVALID_STATE, TAG, "LVGL fs letter already in use");

    lv_fs_drv_init(&s_lvgl.fs_drv);
    s_lvgl.fs_drv.letter = LUA_MODULE_LVGL_FS_LETTER;
    s_lvgl.fs_drv.ready_cb = lua_lvgl_fs_ready_cb;
    s_lvgl.fs_drv.open_cb = lua_lvgl_fs_open_cb;
    s_lvgl.fs_drv.close_cb = lua_lvgl_fs_close_cb;
    s_lvgl.fs_drv.read_cb = lua_lvgl_fs_read_cb;
    s_lvgl.fs_drv.seek_cb = lua_lvgl_fs_seek_cb;
    s_lvgl.fs_drv.tell_cb = lua_lvgl_fs_tell_cb;
    lv_fs_drv_register(&s_lvgl.fs_drv);
    s_lvgl.fs_registered = true;
    return ESP_OK;
}

lua_lvgl_font_ud_t *lua_lvgl_check_font(lua_State *L, int index)
{
    return (lua_lvgl_font_ud_t *)luaL_checkudata(L, index, LUA_LVGL_FONT_MT);
}

lv_font_t *lua_lvgl_validate_font_locked(const lua_lvgl_font_ud_t *ud, const char **out_error)
{
    lua_lvgl_font_record_t *record = ud ? ud->record : NULL;

    if (!s_lvgl.runtime_initialized) {
        *out_error = "lvgl runtime is not initialized";
        return NULL;
    }
    if (!record || record->generation != s_lvgl.generation) {
        *out_error = "lvgl font belongs to an old runtime";
        return NULL;
    }
    if (!record->valid || !record->font) {
        *out_error = "lvgl font has been deleted";
        return NULL;
    }
    return record->font;
}

static void lua_lvgl_release_font_record(lua_lvgl_font_record_t *record)
{
    if (!record || !record->valid) {
        return;
    }
    if (record->font) {
        lv_tiny_ttf_destroy(record->font);
        record->font = NULL;
    }
    record->valid = false;
}

static void lua_lvgl_destroy_font_record(lua_lvgl_font_record_t *record)
{
    if (!record) {
        return;
    }
    lua_lvgl_release_font_record(record);
    if (record->ud) {
        record->ud->record = NULL;
        record->ud = NULL;
    }
    free(record);
}

static lua_lvgl_font_record_t *lua_lvgl_unlink_font_record_locked(lua_lvgl_font_record_t *target)
{
    lua_lvgl_font_record_t **cursor = &s_lvgl.fonts;

    while (*cursor) {
        lua_lvgl_font_record_t *record = *cursor;

        if (record == target) {
            *cursor = record->next;
            record->next = NULL;
            return record;
        }
        cursor = &record->next;
    }
    return NULL;
}

void lua_lvgl_release_fonts_locked(void)
{
    lua_lvgl_font_record_t **cursor = &s_lvgl.fonts;

    while (*cursor) {
        lua_lvgl_font_record_t *record = *cursor;
        if (record->generation == s_lvgl.generation) {
            *cursor = record->next;
            record->next = NULL;
            lua_lvgl_destroy_font_record(record);
        } else {
            cursor = &record->next;
        }
    }
}

static int lua_lvgl_font_load(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    int size = 16;
    int cache_size = LV_TINY_TTF_CACHE_GLYPH_CNT;
    char lv_path[LUA_MODULE_LVGL_PATH_MAX];
    lv_font_t *font;
    lua_lvgl_font_record_t *record;
    lua_lvgl_font_ud_t *ud;
    esp_err_t err;

    if (lua_lvgl_opt_table(L, 2)) {
        size = lua_lvgl_get_opt_int_field(L, 2, "size", size);
        cache_size = lua_lvgl_get_opt_int_field(L, 2, "cache_size", cache_size);
    }
    luaL_argcheck(L, size > 0, 2, "font size must be positive");
    luaL_argcheck(L, cache_size >= 0, 2, "cache_size must be non-negative");

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (!s_lvgl.runtime_initialized) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl runtime is not initialized");
    }
    err = lua_lvgl_to_lv_fs_path(path, lv_path, sizeof(lv_path));
    if (err != ESP_OK) {
        lua_lvgl_unlock();
        if (err == ESP_ERR_INVALID_ARG) {
            return luaL_error(L, "font path must be relative to data root or under %s", s_lvgl.data_root);
        }
        return lua_lvgl_error_esp(L, "font path", err);
    }
    font = lv_tiny_ttf_create_file_ex(lv_path, size, LV_FONT_KERNING_NORMAL, (size_t)cache_size);
    if (!font) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl failed to load font: %s", lv_path);
    }
    record = (lua_lvgl_font_record_t *)calloc(1, sizeof(*record));
    if (!record) {
        lv_tiny_ttf_destroy(font);
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl font record allocation failed");
    }
    record->font = font;
    record->generation = s_lvgl.generation;
    record->valid = true;
    record->next = s_lvgl.fonts;
    s_lvgl.fonts = record;

    ud = (lua_lvgl_font_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->record = record;
    record->ud = ud;
    luaL_getmetatable(L, LUA_LVGL_FONT_MT);
    lua_setmetatable(L, -2);
    lua_lvgl_unlock();
    return 1;
}

static int lua_lvgl_font_delete(lua_State *L)
{
    lua_lvgl_font_ud_t *ud = lua_lvgl_check_font(L, 1);
    esp_err_t err = lua_lvgl_lock();

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (ud->record && ud->record->generation == s_lvgl.generation) {
        lua_lvgl_font_record_t *record = lua_lvgl_unlink_font_record_locked(ud->record);
        if (record) {
            lua_lvgl_destroy_font_record(record);
        } else {
            ud->record = NULL;
        }
    }
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_font_set_size(lua_State *L)
{
    lua_lvgl_font_ud_t *ud = lua_lvgl_check_font(L, 1);
    int size = (int)luaL_checkinteger(L, 2);
    esp_err_t err = lua_lvgl_lock();
    lv_font_t *font;
    const char *font_error = NULL;

    luaL_argcheck(L, size > 0, 2, "font size must be positive");
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    font = lua_lvgl_validate_font_locked(ud, &font_error);
    if (!font) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", font_error);
    }
    lv_tiny_ttf_set_size(font, size);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_font_is_valid(lua_State *L)
{
    lua_lvgl_font_ud_t *ud = lua_lvgl_check_font(L, 1);
    bool valid = false;

    if (lua_lvgl_lock() == ESP_OK) {
        valid = s_lvgl.runtime_initialized &&
                ud->record &&
                ud->record->generation == s_lvgl.generation &&
                ud->record->valid &&
                ud->record->font;
        lua_lvgl_unlock();
    }
    lua_pushboolean(L, valid);
    return 1;
}

static int lua_lvgl_font_gc(lua_State *L)
{
    lua_lvgl_font_ud_t *ud = (lua_lvgl_font_ud_t *)lua_touserdata(L, 1);

    if (ud) {
        if (ud->record && ud->record->ud == ud) {
            ud->record->ud = NULL;
        }
        ud->record = NULL;
    }
    return 0;
}

void lua_lvgl_apply_font_style_field(lua_State *L, int index, lv_obj_t *obj)
{
    lua_lvgl_font_ud_t *font_ud;
    lv_font_t *font;
    const char *font_error = NULL;

    lua_getfield(L, index, "font");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    font_ud = lua_lvgl_check_font(L, -1);
    font = lua_lvgl_validate_font_locked(font_ud, &font_error);
    if (!font) {
        lua_pop(L, 1);
        luaL_error(L, "%s", font_error);
    }
    lv_obj_set_style_text_font(obj, font, 0);
    lua_pop(L, 1);
}

void lua_lvgl_register_font_metatable(lua_State *L)
{
    static const luaL_Reg font_methods[] = {
        {"delete", lua_lvgl_font_delete},
        {"set_size", lua_lvgl_font_set_size},
        {"is_valid", lua_lvgl_font_is_valid},
        {NULL, NULL},
    };

    if (luaL_newmetatable(L, LUA_LVGL_FONT_MT)) {
        lua_newtable(L);
        luaL_setfuncs(L, font_methods, 0);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_lvgl_font_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushstring(L, LUA_LVGL_FONT_MT);
        lua_setfield(L, -2, "__name");
    }
    lua_pop(L, 1);
}

const luaL_Reg lua_lvgl_font_module_funcs[] = {
    {"font_load", lua_lvgl_font_load},
    {NULL, NULL},
};

#else

esp_err_t lua_lvgl_set_data_root(const char *data_root)
{
    (void)data_root;
    return ESP_OK;
}

esp_err_t lua_lvgl_register_fs_locked(void)
{
    return ESP_OK;
}

void lua_lvgl_register_font_metatable(lua_State *L)
{
    (void)L;
}

void lua_lvgl_release_fonts_locked(void)
{
}

lua_lvgl_font_ud_t *lua_lvgl_check_font(lua_State *L, int index)
{
    luaL_argerror(L, index, "LVGL tiny_ttf is disabled");
    return NULL;
}

lv_font_t *lua_lvgl_validate_font_locked(const lua_lvgl_font_ud_t *ud, const char **out_error)
{
    (void)ud;
    *out_error = "LVGL tiny_ttf is disabled";
    return NULL;
}

void lua_lvgl_apply_font_style_field(lua_State *L, int index, lv_obj_t *obj)
{
    (void)L;
    (void)index;
    (void)obj;
}

static int lua_lvgl_font_load_disabled(lua_State *L)
{
    return luaL_error(L, "LVGL tiny_ttf is disabled");
}

const luaL_Reg lua_lvgl_font_module_funcs[] = {
    {"font_load", lua_lvgl_font_load_disabled},
    {NULL, NULL},
};

#endif
