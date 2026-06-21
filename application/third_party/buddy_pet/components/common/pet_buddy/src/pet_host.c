/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pet_host.h"

#include <stdlib.h>

#include "esp_check.h"
#include "pet_renderer.h"

static const char *TAG = "pet_host";

typedef struct {
    pet_renderer_t renderer;
} pet_host_impl_t;

static pet_renderer_page_t pet_host_map_page(pet_host_page_t page)
{
    return page == PET_HOST_PAGE_DETAIL ? PET_RENDERER_PAGE_DETAIL : PET_RENDERER_PAGE_HOME;
}

static pet_host_impl_t *pet_host_impl(pet_host_t *host)
{
    return host != NULL && host->opened ? (pet_host_impl_t *)host->impl : NULL;
}

esp_err_t pet_host_open(pet_host_t *host, const pet_host_config_t *config)
{
    pet_renderer_config_t renderer_config = {0};
    pet_host_impl_t *impl;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(host != NULL && config != NULL && config->display != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid host config");

    memset(host, 0, sizeof(*host));
    impl = calloc(1, sizeof(*impl));
    ESP_RETURN_ON_FALSE(impl != NULL, ESP_ERR_NO_MEM, TAG, "alloc host failed");

    renderer_config.display = config->display;
    renderer_config.max_action_asset_bytes = config->max_action_asset_bytes;

    ret = pet_renderer_create(&impl->renderer, &renderer_config);
    ESP_GOTO_ON_ERROR(ret, fail_free, TAG, "create pet renderer failed");
    host->impl = impl;
    host->opened = true;
    return ESP_OK;

fail_free:
    free(impl);
    memset(host, 0, sizeof(*host));
    return ret;
}

void pet_host_close(pet_host_t *host)
{
    if (host == NULL) {
        return;
    }
    if (host->opened) {
        pet_host_impl_t *impl = (pet_host_impl_t *)host->impl;
        if (impl != NULL) {
            pet_renderer_destroy(&impl->renderer);
            free(impl);
        }
    }
    memset(host, 0, sizeof(*host));
}

esp_err_t pet_host_set_action(pet_host_t *host, const char *action, bool keep_pos)
{
    pet_host_impl_t *impl = pet_host_impl(host);

    if (impl == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return pet_renderer_set_action(&impl->renderer, action, keep_pos);
}

void pet_host_set_visible(pet_host_t *host, bool visible)
{
    pet_host_impl_t *impl = pet_host_impl(host);

    if (impl != NULL) {
        pet_renderer_set_visible(&impl->renderer, visible);
    }
}

void pet_host_place(pet_host_t *host, pet_host_page_t page)
{
    pet_host_impl_t *impl = pet_host_impl(host);

    if (impl != NULL) {
        pet_renderer_place(&impl->renderer, pet_host_map_page(page));
    }
}

void pet_host_handle_touch(pet_host_t *host, const gfx_touch_event_t *event)
{
    pet_host_impl_t *impl = pet_host_impl(host);

    if (impl != NULL) {
        pet_renderer_handle_touch(&impl->renderer, event);
    }
}

gfx_obj_t *pet_host_object(pet_host_t *host)
{
    pet_host_impl_t *impl = pet_host_impl(host);

    return impl != NULL ? pet_renderer_object(&impl->renderer) : NULL;
}
