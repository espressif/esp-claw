/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_pet.h"

#include <stdlib.h>

#include "cap_lua.h"
#include "claw_event_publisher.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "pet_buddy.h"
#include "pet_registry.h"

static const char *TAG = "lua_pet";

static int lua_pet_push_result(lua_State *L, esp_err_t ret)
{
    if (ret == ESP_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }

    return luaL_error(L, "%s", esp_err_to_name(ret));
}

static void lua_pet_publish_selection_event(const char *event_key, const pet_registry_entry_t *entry)
{
    cJSON *root = NULL;
    char *payload_json = NULL;
    esp_err_t ret;

    root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGW(TAG, "build pet selection event failed: no mem");
        return;
    }

    if (entry != NULL) {
        cJSON_AddBoolToObject(root, "selected", true);
        cJSON_AddStringToObject(root, "id", entry->id);
        cJSON_AddStringToObject(root, "name", entry->name);
        cJSON_AddStringToObject(root, "title", entry->title);
        cJSON_AddStringToObject(root, "profile", entry->profile);
        cJSON_AddStringToObject(root, "asset_path", entry->asset_path);
    } else {
        cJSON_AddBoolToObject(root, "selected", false);
    }

    payload_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload_json == NULL) {
        ESP_LOGW(TAG, "build pet selection event failed: encode");
        return;
    }

    ret = claw_event_router_publish_trigger("pet_buddy", "pet", event_key, payload_json);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "publish pet %s event failed: %s", event_key, esp_err_to_name(ret));
    }
    free(payload_json);
}

static int lua_pet_refresh(lua_State *L)
{
    return lua_pet_push_result(L, pet_registry_refresh());
}

static int lua_pet_select(lua_State *L)
{
    const char *pet_id = luaL_checkstring(L, 1);
    pet_registry_entry_t entry;
    esp_err_t ret;

    if (pet_id == NULL || pet_id[0] == '\0') {
        ret = pet_buddy_clear_selected();
        if (ret == ESP_OK) {
            lua_pet_publish_selection_event("cleared", NULL);
        }
        return lua_pet_push_result(L, ret);
    }
    ret = pet_buddy_select(pet_id);
    if (ret == ESP_OK && pet_registry_get_selected_entry(&entry) == ESP_OK) {
        lua_pet_publish_selection_event("selected", &entry);
    }
    return lua_pet_push_result(L, ret);
}

static int lua_pet_clear(lua_State *L)
{
    esp_err_t ret = pet_buddy_clear_selected();

    if (ret == ESP_OK) {
        lua_pet_publish_selection_event("cleared", NULL);
    }
    return lua_pet_push_result(L, ret);
}

static int lua_pet_current(lua_State *L)
{
    char pet_id[PET_REGISTRY_ID_LEN];

    if (pet_registry_get_selected(pet_id, sizeof(pet_id)) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, pet_id);
    return 1;
}

static int lua_pet_is_running(lua_State *L)
{
    lua_pushboolean(L, pet_buddy_has_active());
    return 1;
}

static int lua_pet_active(lua_State *L)
{
    lua_pushboolean(L, pet_buddy_has_active());
    return 1;
}

static int lua_pet_action(lua_State *L)
{
    const char *action = luaL_checkstring(L, 1);

    return lua_pet_push_result(L, pet_buddy_action(action, true));
}

static int lua_pet_list(lua_State *L)
{
    size_t count;

    (void)pet_registry_refresh();
    count = pet_registry_count();
    lua_createtable(L, (int)count, 0);
    for (size_t i = 0; i < count; i++) {
        pet_registry_entry_t entry;

        if (pet_registry_get(i, &entry) != ESP_OK) {
            continue;
        }

        lua_createtable(L, 0, 7);
        lua_pushstring(L, entry.id);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, entry.name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, entry.title);
        lua_setfield(L, -2, "title");
        lua_pushstring(L, entry.profile);
        lua_setfield(L, -2, "profile");
        lua_pushstring(L, entry.profile);
        lua_setfield(L, -2, "country");
        lua_pushstring(L, entry.base_path);
        lua_setfield(L, -2, "path");
        lua_pushstring(L, entry.asset_path);
        lua_setfield(L, -2, "asset");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

int luaopen_pet(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_pet_list);
    lua_setfield(L, -2, "list");
    lua_pushcfunction(L, lua_pet_select);
    lua_setfield(L, -2, "select");
    lua_pushcfunction(L, lua_pet_clear);
    lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, lua_pet_current);
    lua_setfield(L, -2, "current");
    lua_pushcfunction(L, lua_pet_refresh);
    lua_setfield(L, -2, "refresh");
    lua_pushcfunction(L, lua_pet_is_running);
    lua_setfield(L, -2, "is_running");
    lua_pushcfunction(L, lua_pet_active);
    lua_setfield(L, -2, "active");
    lua_pushcfunction(L, lua_pet_action);
    lua_setfield(L, -2, "action");
    return 1;
}

esp_err_t lua_module_pet_register(const char *fatfs_base_path)
{
    if (fatfs_base_path != NULL && fatfs_base_path[0] != '\0') {
        (void)pet_registry_set_fatfs_base_path(fatfs_base_path);
    }
    return cap_lua_register_module("pet", luaopen_pet);
}
