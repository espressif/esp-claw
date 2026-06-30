/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lauxlib.h"
#include "cap_lua.h"
#include "claw_paths.h"
#include "esp_vfs_fat.h"

/* The writable data root is owned by claw_paths; resolve it on each call so the
 * module always sees the current mount point (flash partition or SD card). */
static const char *lua_module_storage_base_path(void)
{
    return claw_paths_get(CLAW_PATH_DATA);
}

static int lua_module_storage_get_root_dir(lua_State *L)
{
    const char *base_path = lua_module_storage_base_path();

    if (!base_path || !base_path[0]) {
        return luaL_error(L, "storage root is not configured");
    }
    lua_pushstring(L, base_path);
    return 1;
}

static const char *lua_module_storage_stat_type(mode_t mode)
{
    if (S_ISDIR(mode)) {
        return "dir";
    }
    if (S_ISREG(mode)) {
        return "file";
    }
    return "other";
}

static void lua_module_storage_push_stat_table(lua_State *L, const char *name, const struct stat *st)
{
    lua_newtable(L);
    if (name) {
        lua_pushstring(L, name);
        lua_setfield(L, -2, "name");
    }
    lua_pushstring(L, lua_module_storage_stat_type(st->st_mode));
    lua_setfield(L, -2, "type");
    lua_pushinteger(L, (lua_Integer)st->st_size);
    lua_setfield(L, -2, "size");
    lua_pushinteger(L, (lua_Integer)st->st_mtime);
    lua_setfield(L, -2, "mtime");
    lua_pushinteger(L, (lua_Integer)st->st_mode);
    lua_setfield(L, -2, "mode");
}

static int lua_module_storage_join_path(lua_State *L)
{
    int argc = lua_gettop(L);
    luaL_Buffer buf;
    bool wrote = false;
    bool ends_with_slash = false;

    luaL_buffinit(L, &buf);

    for (int i = 1; i <= argc; i++) {
        size_t len = 0;
        size_t start = 0;
        size_t end = 0;
        const char *part = luaL_checklstring(L, i, &len);

        if (len == 0) {
            continue;
        }

        end = len;
        if (wrote) {
            while (start < end && part[start] == '/') {
                start++;
            }
        }
        while (end > start + 1 && part[end - 1] == '/') {
            end--;
        }
        if (end <= start) {
            continue;
        }

        if (wrote && !ends_with_slash) {
            luaL_addchar(&buf, '/');
        }
        luaL_addlstring(&buf, part + start, end - start);
        wrote = true;
        ends_with_slash = part[end - 1] == '/';
    }

    luaL_pushresult(&buf);
    return 1;
}

static int lua_module_storage_exists(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    struct stat st;

    lua_pushboolean(L, stat(path, &st) == 0);
    return 1;
}

static int lua_module_storage_stat(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    struct stat st;

    if (stat(path, &st) != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "stat failed for %s: %s", path, strerror(errno));
        return 2;
    }

    lua_module_storage_push_stat_table(L, NULL, &st);
    return 1;
}

static int lua_module_storage_mkdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return luaL_error(L, "mkdir failed for %s", path);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_module_storage_write_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t content_len = 0;
    const char *content = luaL_checklstring(L, 2, &content_len);
    FILE *file = NULL;

    file = fopen(path, "w");
    if (!file) {
        return luaL_error(L, "cannot open file for writing: %s", path);
    }

    if (fwrite(content, 1, content_len, file) != content_len) {
        fclose(file);
        return luaL_error(L, "short write to %s", path);
    }

    fclose(file);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_module_storage_read_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    FILE *file = NULL;
    long size = 0;
    char *buf = NULL;

    file = fopen(path, "rb");
    if (!file) {
        return luaL_error(L, "cannot open file for reading: %s", path);
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return luaL_error(L, "seek failed for %s", path);
    }

    size = ftell(file);
    if (size < 0) {
        fclose(file);
        return luaL_error(L, "tell failed for %s", path);
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return luaL_error(L, "seek failed for %s", path);
    }

    buf = calloc(1, (size_t)size + 1);
    if (!buf) {
        fclose(file);
        return luaL_error(L, "failed to allocate read buffer");
    }

    if (size > 0 && fread(buf, 1, (size_t)size, file) != (size_t)size) {
        free(buf);
        fclose(file);
        return luaL_error(L, "read failed for %s", path);
    }

    fclose(file);
    lua_pushlstring(L, buf, (size_t)size);
    free(buf);
    return 1;
}

static int lua_module_storage_listdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    int index = 1;

    dir = opendir(path);
    if (!dir) {
        return luaL_error(L, "opendir failed for %s: %s", path, strerror(errno));
    }

    lua_newtable(L);
    while ((entry = readdir(dir)) != NULL) {
        size_t path_len = strlen(path);
        size_t name_len = strlen(entry->d_name);
        size_t full_len = path_len + 1 + name_len + 1;
        char *full_path = NULL;
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        full_path = malloc(full_len);
        if (!full_path) {
            closedir(dir);
            return luaL_error(L, "failed to allocate listdir path buffer");
        }

        if (path_len > 0 && path[path_len - 1] == '/') {
            snprintf(full_path, full_len, "%s%s", path, entry->d_name);
        } else {
            snprintf(full_path, full_len, "%s/%s", path, entry->d_name);
        }

        if (stat(full_path, &st) == 0) {
            lua_module_storage_push_stat_table(L, entry->d_name, &st);
        } else {
            lua_newtable(L);
            lua_pushstring(L, entry->d_name);
            lua_setfield(L, -2, "name");
            lua_pushstring(L, "unknown");
            lua_setfield(L, -2, "type");
        }
        lua_rawseti(L, -2, index++);
        free(full_path);
    }

    closedir(dir);
    return 1;
}

static int lua_module_storage_remove(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    if (remove(path) != 0) {
        return luaL_error(L, "remove failed for %s: %s", path, strerror(errno));
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_module_storage_rename(lua_State *L)
{
    const char *old_path = luaL_checkstring(L, 1);
    const char *new_path = luaL_checkstring(L, 2);

    if (rename(old_path, new_path) != 0) {
        return luaL_error(L, "rename failed from %s to %s: %s", old_path, new_path, strerror(errno));
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_module_storage_get_free_space(lua_State *L)
{
    const char *base_path = lua_module_storage_base_path();
    uint64_t total = 0;
    uint64_t free_bytes = 0;

    if (!base_path || !base_path[0]) {
        return luaL_error(L, "storage root is not configured");
    }

    esp_err_t err = esp_vfs_fat_info(base_path, &total, &free_bytes);

    if (err != ESP_OK) {
        return luaL_error(L, "failed to query storage free space: %s", esp_err_to_name(err));
    }

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)total);
    lua_setfield(L, -2, "total");
    lua_pushinteger(L, (lua_Integer)free_bytes);
    lua_setfield(L, -2, "free");
    lua_pushinteger(L, (lua_Integer)(total - free_bytes));
    lua_setfield(L, -2, "used");
    return 1;
}

int luaopen_storage(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_module_storage_get_root_dir);
    lua_setfield(L, -2, "get_root_dir");
    lua_pushcfunction(L, lua_module_storage_join_path);
    lua_setfield(L, -2, "join_path");
    lua_pushcfunction(L, lua_module_storage_exists);
    lua_setfield(L, -2, "exists");
    lua_pushcfunction(L, lua_module_storage_stat);
    lua_setfield(L, -2, "stat");
    lua_pushcfunction(L, lua_module_storage_mkdir);
    lua_setfield(L, -2, "mkdir");
    lua_pushcfunction(L, lua_module_storage_write_file);
    lua_setfield(L, -2, "write_file");
    lua_pushcfunction(L, lua_module_storage_read_file);
    lua_setfield(L, -2, "read_file");
    lua_pushcfunction(L, lua_module_storage_listdir);
    lua_setfield(L, -2, "listdir");
    lua_pushcfunction(L, lua_module_storage_remove);
    lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, lua_module_storage_rename);
    lua_setfield(L, -2, "rename");
    lua_pushcfunction(L, lua_module_storage_get_free_space);
    lua_setfield(L, -2, "get_free_space");
    return 1;
}

esp_err_t lua_module_storage_register(void)
{
    return cap_lua_register_module("storage", luaopen_storage);
}
