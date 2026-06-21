/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pet_renderer.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hw_gfx_runtime.h"
#include "pet_registry.h"

#define PET_RENDERER_INDEX_JSON_NAME    "index.json"
#define PET_RENDERER_ACTION_ASSET_EXT   ".eaf"
#define PET_RENDERER_ACTION_MAX         16
#define PET_RENDERER_INDEX_STR_LEN      48
#define PET_RENDERER_TOUCH_THRESHOLD_PX 8

typedef struct {
    char name[PET_RENDERER_INDEX_STR_LEN];
    char file[PET_RENDERER_INDEX_STR_LEN];
    int asset_id;
} pet_renderer_action_t;

typedef struct {
    gfx_obj_t *anim_obj;
    gfx_disp_t *display;
    mmap_assets_handle_t assets_handle;
    bool owns_assets_handle;
    size_t max_action_asset_bytes;
    pet_renderer_action_t actions[PET_RENDERER_ACTION_MAX];
    size_t action_count;
    char active_action[PET_RENDERER_INDEX_STR_LEN];
    void *active_data;
    SemaphoreHandle_t lock;
    bool touch_tracking;
    int32_t touch_press_x;
    int32_t touch_press_y;
    gfx_coord_t anim_press_x;
    gfx_coord_t anim_press_y;
} pet_renderer_impl_t;

static const char *const TAG = "pet_renderer";

static void pet_renderer_copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static int pet_renderer_find_asset_id_by_name(mmap_assets_handle_t assets_handle, const char *filename)
{
    if (assets_handle == NULL || filename == NULL || filename[0] == '\0') {
        return -1;
    }

    for (int i = 0; i < mmap_assets_get_stored_files(assets_handle); i++) {
        const char *name = mmap_assets_get_name(assets_handle, i);
        if (name != NULL && strcmp(name, filename) == 0) {
            return i;
        }
    }
    return -1;
}

static bool pet_renderer_name_has_suffix(const char *name, const char *suffix)
{
    size_t name_len;
    size_t suffix_len;

    if (name == NULL || suffix == NULL) {
        return false;
    }
    name_len = strlen(name);
    suffix_len = strlen(suffix);
    return name_len >= suffix_len && strcmp(name + name_len - suffix_len, suffix) == 0;
}

static void pet_renderer_copy_stem(char *dst, size_t dst_size, const char *filename)
{
    const char *base;
    const char *dot;
    size_t len;

    if (dst == NULL || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (filename == NULL) {
        return;
    }

    base = strrchr(filename, '/');
    base = base != NULL ? base + 1 : filename;
    dot = strrchr(base, '.');
    len = dot != NULL ? (size_t)(dot - base) : strlen(base);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, base, len);
    dst[len] = '\0';
}

static esp_err_t pet_renderer_load_index_from_assets(pet_renderer_impl_t *impl)
{
    int files;

    if (impl == NULL || impl->assets_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    files = mmap_assets_get_stored_files(impl->assets_handle);
    impl->action_count = 0;
    for (int i = 0; i < files && impl->action_count < PET_RENDERER_ACTION_MAX; i++) {
        const char *name = mmap_assets_get_name(impl->assets_handle, i);
        pet_renderer_action_t *entry;

        if (name == NULL || !pet_renderer_name_has_suffix(name, PET_RENDERER_ACTION_ASSET_EXT)) {
            continue;
        }

        entry = &impl->actions[impl->action_count];
        memset(entry, 0, sizeof(*entry));
        pet_renderer_copy_str(entry->file, sizeof(entry->file), name);
        pet_renderer_copy_stem(entry->name, sizeof(entry->name), name);
        entry->asset_id = i;
        impl->action_count++;
    }

    ESP_LOGW(TAG, "index.json unavailable, fallback to %u pet bin entries", (unsigned)impl->action_count);
    return impl->action_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool pet_renderer_name_has_action_suffix(const char *name, const char *action)
{
    const char *base;
    const char *dot;
    size_t stem_len;
    size_t action_len;

    if (name == NULL || action == NULL) {
        return false;
    }

    base = strrchr(name, '/');
    base = (base != NULL) ? base + 1 : name;
    dot = strrchr(base, '.');
    stem_len = (dot != NULL) ? (size_t)(dot - base) : strlen(base);
    action_len = strlen(action);

    if (stem_len < action_len) {
        return false;
    }
    if (strncmp(base + stem_len - action_len, action, action_len) != 0) {
        return false;
    }
    return stem_len == action_len || base[stem_len - action_len - 1] == '-';
}

static void *pet_renderer_copy_asset(pet_renderer_impl_t *impl,
                                     int asset_id,
                                     const char *asset_label,
                                     bool enforce_action_limit,
                                     size_t *out_len)
{
    int asset_size;
    size_t max_size;
    void *data;
    size_t copied;
    const uint8_t *asset_mem;

    if (out_len != NULL) {
        *out_len = 0;
    }
    if (impl == NULL || impl->assets_handle == NULL || asset_id < 0) {
        return NULL;
    }

    asset_size = mmap_assets_get_size(impl->assets_handle, asset_id);
    if (asset_size <= 0) {
        return NULL;
    }
    max_size = impl->max_action_asset_bytes > 0 ? impl->max_action_asset_bytes :
               PET_RENDERER_DEFAULT_ACTION_ASSET_MAX_BYTES;
    if (enforce_action_limit && (size_t)asset_size > max_size) {
        ESP_LOGW(TAG, "action asset too large: asset=%s size=%d max=%u",
                 asset_label != NULL ? asset_label : "(unknown)",
                 asset_size,
                 (unsigned)max_size);
        return NULL;
    }
    asset_mem = mmap_assets_get_mem(impl->assets_handle, asset_id);
    if (asset_mem == NULL) {
        return NULL;
    }

    data = heap_caps_malloc((size_t)asset_size + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        data = heap_caps_malloc((size_t)asset_size + 1U, MALLOC_CAP_8BIT);
    }
    if (data == NULL) {
        return NULL;
    }

    copied = mmap_assets_copy_mem(impl->assets_handle, (size_t)asset_mem, data, (size_t)asset_size);
    if (copied == 0) {
        free(data);
        return NULL;
    }
    ((char *)data)[copied] = '\0';
    if (out_len != NULL) {
        *out_len = copied;
    }
    return data;
}

static esp_err_t pet_renderer_load_index(pet_renderer_impl_t *impl)
{
    int index_id;
    void *json_data;
    size_t json_len;
    cJSON *root;
    cJSON *item = NULL;

    if (impl == NULL || impl->assets_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    index_id = pet_renderer_find_asset_id_by_name(impl->assets_handle, PET_RENDERER_INDEX_JSON_NAME);
    if (index_id < 0) {
        ESP_LOGW(TAG, "%s not found in pet bin", PET_RENDERER_INDEX_JSON_NAME);
        return pet_renderer_load_index_from_assets(impl);
    }

    json_data = pet_renderer_copy_asset(impl, index_id, PET_RENDERER_INDEX_JSON_NAME, false, &json_len);
    if (json_data == NULL || json_len == 0) {
        ESP_LOGW(TAG, "%s has no data", PET_RENDERER_INDEX_JSON_NAME);
        return pet_renderer_load_index_from_assets(impl);
    }
    root = cJSON_ParseWithLength((const char *)json_data, json_len);
    free(json_data);
    if (root == NULL) {
        ESP_LOGW(TAG, "failed to parse %s", PET_RENDERER_INDEX_JSON_NAME);
        return pet_renderer_load_index_from_assets(impl);
    }
    if (!cJSON_IsArray(root)) {
        ESP_LOGW(TAG, "%s root must be a JSON array", PET_RENDERER_INDEX_JSON_NAME);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    impl->action_count = 0;
    cJSON_ArrayForEach(item, root) {
        cJSON *jn;
        cJSON *jf;
        pet_renderer_action_t *entry;

        if (impl->action_count >= PET_RENDERER_ACTION_MAX) {
            ESP_LOGW(TAG, "index truncated at %u entries", PET_RENDERER_ACTION_MAX);
            break;
        }

        jn = cJSON_GetObjectItem(item, "name");
        jf = cJSON_GetObjectItem(item, "file");
        entry = &impl->actions[impl->action_count];
        memset(entry, 0, sizeof(*entry));
        entry->asset_id = -1;

        if (cJSON_IsString(jn) && jn->valuestring != NULL) {
            pet_renderer_copy_str(entry->name, sizeof(entry->name), jn->valuestring);
        }
        if (cJSON_IsString(jf) && jf->valuestring != NULL) {
            pet_renderer_copy_str(entry->file, sizeof(entry->file), jf->valuestring);
        }
        entry->asset_id = pet_renderer_find_asset_id_by_name(impl->assets_handle, entry->file);
        if (entry->asset_id < 0) {
            ESP_LOGW(TAG, "index: pet bin has no file \"%s\" (name=%s)", entry->file, entry->name);
        }
        impl->action_count++;
    }

    cJSON_Delete(root);
    ESP_LOGD(TAG, "index.json: loaded %u entries from pet bin", (unsigned)impl->action_count);
    return impl->action_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static const pet_renderer_action_t *pet_renderer_find_fallback_action(pet_renderer_impl_t *impl)
{
    if (impl == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < impl->action_count; i++) {
        if (strcmp(impl->actions[i].name, PET_RENDERER_ACTION_LOSE) == 0 && impl->actions[i].asset_id >= 0) {
            return &impl->actions[i];
        }
    }
    for (size_t i = 0; i < impl->action_count; i++) {
        if (impl->actions[i].asset_id >= 0) {
            return &impl->actions[i];
        }
    }
    return NULL;
}

static const pet_renderer_action_t *pet_renderer_find_action(pet_renderer_impl_t *impl, const char *action)
{
    if (impl == NULL || action == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < impl->action_count; i++) {
        if (strcmp(impl->actions[i].name, action) == 0 || strcmp(impl->actions[i].file, action) == 0) {
            return &impl->actions[i];
        }
    }
    for (size_t i = 0; i < impl->action_count; i++) {
        if (pet_renderer_name_has_action_suffix(impl->actions[i].name, action) ||
            pet_renderer_name_has_action_suffix(impl->actions[i].file, action)) {
            return &impl->actions[i];
        }
    }
    if (strcmp(action, PET_RENDERER_ACTION_RUNNING_LEFT) == 0 ||
        strcmp(action, PET_RENDERER_ACTION_RUNNING_RIGHT) == 0) {
        return pet_renderer_find_action(impl, PET_RENDERER_ACTION_RUNNING);
    }
    if (strcmp(action, PET_RENDERER_ACTION_IDLE) != 0) {
        return pet_renderer_find_action(impl, PET_RENDERER_ACTION_IDLE);
    }
    return pet_renderer_find_fallback_action(impl);
}

static void pet_renderer_free_active(pet_renderer_impl_t *impl)
{
    if (impl != NULL && impl->active_data != NULL) {
        free(impl->active_data);
        impl->active_data = NULL;
        impl->active_action[0] = '\0';
    }
}

esp_err_t pet_renderer_set_action(pet_renderer_t *pet, const char *action, bool keep_pos)
{
    pet_renderer_impl_t *impl = pet != NULL ? pet->impl : NULL;
    const pet_renderer_action_t *item;
    void *data;
    size_t data_len;
    gfx_anim_src_t anim_src;
    gfx_coord_t x = 0;
    gfx_coord_t y = 0;
    void *old_data;
    esp_err_t err;

    if (impl == NULL || impl->anim_obj == NULL || action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (impl->lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(impl->lock, portMAX_DELAY);

    item = pet_renderer_find_action(impl, action);
    if (item == NULL || item->asset_id < 0) {
        ESP_LOGW(TAG, "skip action \"%s\"", action);
        err = ESP_ERR_NOT_FOUND;
        goto out_unlock;
    }
    if (strcmp(impl->active_action, item->name) == 0) {
        err = ESP_OK;
        goto out_unlock;
    }

    data = pet_renderer_copy_asset(impl, item->asset_id, item->name, true, &data_len);
    if (data == NULL || data_len == 0) {
        ESP_LOGW(TAG, "load action failed: action=%s", item->name);
        err = ESP_ERR_NO_MEM;
        goto out_unlock;
    }

    if (keep_pos) {
        (void)gfx_obj_get_pos(impl->anim_obj, &x, &y);
    }

    anim_src.type = GFX_ANIM_SRC_TYPE_MEMORY;
    anim_src.data = data;
    anim_src.data_len = data_len;

    old_data = impl->active_data;
    err = hw_gfx_runtime_lock();
    if (err == ESP_OK) {
        err = gfx_anim_stop(impl->anim_obj);
        if (err == ESP_OK) {
            err = gfx_anim_set_src_desc(impl->anim_obj, &anim_src);
        }
        if (err == ESP_OK) {
            err = gfx_anim_set_auto_mirror(impl->anim_obj, false);
        }
        if (err == ESP_OK) {
            err = gfx_anim_set_segment(impl->anim_obj, 0, 0xFFFFFFFF, 4, true);
        }
        if (err == ESP_OK) {
            if (keep_pos) {
                err = gfx_obj_set_pos(impl->anim_obj, x, y);
            } else {
                err = gfx_obj_align(impl->anim_obj, GFX_ALIGN_CENTER, 0, 0);
            }
        }
        if (err == ESP_OK) {
            err = gfx_anim_start(impl->anim_obj);
        }
        hw_gfx_runtime_unlock();
    }

    if (err == ESP_OK) {
        free(old_data);
        impl->active_data = data;
        pet_renderer_copy_str(impl->active_action, sizeof(impl->active_action), item->name);
        ESP_LOGV(TAG, "action: %s", item->name);
    } else {
        free(data);
    }
out_unlock:
    xSemaphoreGive(impl->lock);
    return err;
}

void pet_renderer_destroy(pet_renderer_t *pet)
{
    pet_renderer_impl_t *impl = pet != NULL ? pet->impl : NULL;

    if (impl == NULL) {
        return;
    }
    if (impl->lock != NULL) {
        xSemaphoreTake(impl->lock, portMAX_DELAY);
    }
    if (impl->anim_obj != NULL && hw_gfx_runtime_lock() == ESP_OK) {
        (void)gfx_obj_delete(impl->anim_obj);
        hw_gfx_runtime_unlock();
    }
    pet_renderer_free_active(impl);
    if (impl->owns_assets_handle && impl->assets_handle != NULL) {
        (void)mmap_assets_del(impl->assets_handle);
    }
    if (impl->lock != NULL) {
        xSemaphoreGive(impl->lock);
        vSemaphoreDelete(impl->lock);
    }
    free(impl);
    pet->impl = NULL;
}

esp_err_t pet_renderer_load_package(pet_renderer_t *pet,
                                    mmap_assets_handle_t assets_handle,
                                    bool take_ownership)
{
    pet_renderer_impl_t *impl = pet != NULL ? pet->impl : NULL;
    mmap_assets_handle_t old_assets = NULL;
    bool old_owns_assets = false;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(impl != NULL && assets_handle != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid package args");
    ESP_RETURN_ON_FALSE(impl->lock != NULL, ESP_ERR_INVALID_STATE, TAG, "renderer lock unavailable");

    xSemaphoreTake(impl->lock, portMAX_DELAY);
    pet_renderer_free_active(impl);
    old_assets = impl->assets_handle;
    old_owns_assets = impl->owns_assets_handle;
    impl->assets_handle = assets_handle;
    impl->owns_assets_handle = take_ownership;
    ret = pet_renderer_load_index(impl);
    ESP_GOTO_ON_ERROR(ret, fail_restore_assets, TAG, "load pet index failed");

    if (old_owns_assets && old_assets != NULL && old_assets != assets_handle) {
        (void)mmap_assets_del(old_assets);
    }

    if (impl->anim_obj == NULL) {
        ret = hw_gfx_runtime_lock();
        ESP_GOTO_ON_ERROR(ret, fail_unlock, TAG, "lock gfx failed");
        impl->anim_obj = gfx_anim_create(impl->display);
        if (impl->anim_obj != NULL) {
            (void)gfx_obj_set_visible(impl->anim_obj, false);
        }
        hw_gfx_runtime_unlock();
        ESP_GOTO_ON_FALSE(impl->anim_obj != NULL, ESP_ERR_NO_MEM, fail_unlock, TAG, "create anim failed");
    }
    xSemaphoreGive(impl->lock);

    ret = pet_renderer_set_action(pet, PET_RENDERER_ACTION_IDLE, false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "load initial pet action failed: %s", esp_err_to_name(ret));
    }
    return ret;

fail_restore_assets:
    impl->assets_handle = old_assets;
    impl->owns_assets_handle = old_owns_assets;
    xSemaphoreGive(impl->lock);
    if (take_ownership) {
        (void)mmap_assets_del(assets_handle);
    }
    return ret;

fail_unlock:
    xSemaphoreGive(impl->lock);
    return ret;
}

esp_err_t pet_renderer_create(pet_renderer_t *pet, const pet_renderer_config_t *config)
{
    pet_renderer_impl_t *impl;
    mmap_assets_handle_t assets_handle = NULL;
    char asset_path[PET_REGISTRY_ASSET_PATH_LEN];
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(pet != NULL && config != NULL && config->display != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid renderer config");

    memset(pet, 0, sizeof(*pet));
    impl = heap_caps_calloc(1, sizeof(*impl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (impl == NULL) {
        impl = heap_caps_calloc(1, sizeof(*impl), MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(impl != NULL, ESP_ERR_NO_MEM, TAG, "alloc renderer failed");

    impl->display = config->display;
    impl->max_action_asset_bytes = config->max_action_asset_bytes > 0 ?
                                   config->max_action_asset_bytes :
                                   PET_RENDERER_DEFAULT_ACTION_ASSET_MAX_BYTES;
    impl->lock = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(impl->lock != NULL, ESP_ERR_NO_MEM, fail_free_impl, TAG, "create renderer lock failed");
    pet->impl = impl;

    (void)pet_registry_refresh();
    ret = pet_registry_get_selected_asset_path(asset_path, sizeof(asset_path));
    if (ret == ESP_OK) {
        const mmap_assets_config_t asset_config = {
            .partition_label = asset_path,
            .flags = {
                .use_fs = true,
                .mmap_enable = false,
            }
        };
        ret = mmap_assets_new(&asset_config, &assets_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "open selected pet assets failed: path=%s err=%s",
                     asset_path, esp_err_to_name(ret));
            goto fail_destroy;
        }
        ret = pet_renderer_load_package(pet, assets_handle, true);
        ESP_GOTO_ON_ERROR(ret, fail_destroy, TAG, "load selected pet package failed");
        return ESP_OK;
    }

    ret = pet_registry_get_default_asset_path(asset_path, sizeof(asset_path));
    if (ret == ESP_OK) {
        const mmap_assets_config_t asset_config = {
            .partition_label = asset_path,
            .flags = {
                .use_fs = true,
                .mmap_enable = false,
            }
        };
        ret = mmap_assets_new(&asset_config, &assets_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "open fallback pet assets failed: path=%s err=%s",
                     asset_path, esp_err_to_name(ret));
            goto fail_destroy;
        }
        ret = pet_renderer_load_package(pet, assets_handle, true);
        ESP_GOTO_ON_ERROR(ret, fail_destroy, TAG, "load fallback pet package failed");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "no selected pet and no default pet assets");
    ret = ESP_ERR_NOT_FOUND;

fail_destroy:
    pet_renderer_destroy(pet);
    return ret;

fail_free_impl:
    free(impl);
    return ret;
}

void pet_renderer_set_visible(pet_renderer_t *pet, bool visible)
{
    pet_renderer_impl_t *impl = pet != NULL ? pet->impl : NULL;

    if (impl != NULL && impl->anim_obj != NULL) {
        (void)gfx_obj_set_visible(impl->anim_obj, visible);
    }
}

void pet_renderer_place(pet_renderer_t *pet, pet_renderer_page_t page)
{
    pet_renderer_impl_t *impl = pet != NULL ? pet->impl : NULL;

    if (impl == NULL || impl->anim_obj == NULL) {
        return;
    }
    if (page == PET_RENDERER_PAGE_HOME) {
        (void)gfx_obj_align(impl->anim_obj, GFX_ALIGN_CENTER, 0, 0);
    }
}

static void pet_renderer_follow_touch(pet_renderer_impl_t *impl, const gfx_touch_event_t *event)
{
    gfx_coord_t x;
    gfx_coord_t y;

    if (impl == NULL || event == NULL || impl->anim_obj == NULL) {
        return;
    }

    x = impl->anim_press_x + (gfx_coord_t)((int32_t)event->x - impl->touch_press_x);
    y = impl->anim_press_y + (gfx_coord_t)((int32_t)event->y - impl->touch_press_y);
    if (hw_gfx_runtime_lock() == ESP_OK) {
        (void)gfx_obj_set_pos(impl->anim_obj, x, y);
        hw_gfx_runtime_unlock();
    }
}

void pet_renderer_handle_touch(pet_renderer_t *pet, const gfx_touch_event_t *event)
{
    pet_renderer_impl_t *impl = pet != NULL ? pet->impl : NULL;
    uint16_t anim_w = 0;
    uint16_t anim_h = 0;
    int32_t dx;
    int32_t dy;
    int32_t abs_dx;
    int32_t abs_dy;

    if (impl == NULL || event == NULL || impl->anim_obj == NULL) {
        return;
    }

    switch (event->type) {
    case GFX_TOUCH_EVENT_PRESS:
        if (hw_gfx_runtime_lock() == ESP_OK) {
            (void)gfx_obj_get_size(impl->anim_obj, &anim_w, &anim_h);
            hw_gfx_runtime_unlock();
        }
        impl->touch_tracking = true;
        impl->touch_press_x = event->x;
        impl->touch_press_y = event->y;
        impl->anim_press_x = (gfx_coord_t)event->x - (gfx_coord_t)(anim_w / 2U);
        impl->anim_press_y = (gfx_coord_t)event->y - (gfx_coord_t)anim_h;
        pet_renderer_follow_touch(impl, event);
        break;

    case GFX_TOUCH_EVENT_MOVE:
        if (!impl->touch_tracking) {
            break;
        }

        pet_renderer_follow_touch(impl, event);
        dx = (int32_t)event->x - impl->touch_press_x;
        dy = (int32_t)event->y - impl->touch_press_y;
        abs_dx = dx >= 0 ? dx : -dx;
        abs_dy = dy >= 0 ? dy : -dy;

        if (dy > 0 && abs_dy >= PET_RENDERER_TOUCH_THRESHOLD_PX && abs_dy > abs_dx) {
            (void)pet_renderer_set_action(pet, PET_RENDERER_ACTION_RUNNING, true);
        } else if (abs_dx >= PET_RENDERER_TOUCH_THRESHOLD_PX) {
            (void)pet_renderer_set_action(pet, dx < 0 ? PET_RENDERER_ACTION_RUNNING_LEFT : PET_RENDERER_ACTION_RUNNING_RIGHT, true);
        }
        break;

    case GFX_TOUCH_EVENT_RELEASE:
    default:
        if (impl->touch_tracking) {
            pet_renderer_follow_touch(impl, event);
            impl->touch_tracking = false;
        }
        (void)pet_renderer_set_action(pet, PET_RENDERER_ACTION_IDLE, true);
        break;
    }
}

gfx_obj_t *pet_renderer_object(pet_renderer_t *pet)
{
    pet_renderer_impl_t *impl = pet != NULL ? pet->impl : NULL;

    return impl != NULL ? impl->anim_obj : NULL;
}

void pet_renderer_tick(pet_renderer_t *pet, uint32_t now_ms)
{
    (void)pet;
    (void)now_ms;
}

esp_err_t pet_renderer_draw_frame(pet_renderer_t *pet)
{
    return (pet != NULL && pet->impl != NULL) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void pet_renderer_get_preferred_size(pet_renderer_t *pet, uint16_t *width, uint16_t *height)
{
    pet_renderer_impl_t *impl = pet != NULL ? pet->impl : NULL;

    if (width != NULL) {
        *width = 0;
    }
    if (height != NULL) {
        *height = 0;
    }
    if (impl == NULL || impl->anim_obj == NULL) {
        return;
    }
    if (hw_gfx_runtime_lock() == ESP_OK) {
        (void)gfx_obj_get_size(impl->anim_obj, width, height);
        hw_gfx_runtime_unlock();
    }
}
