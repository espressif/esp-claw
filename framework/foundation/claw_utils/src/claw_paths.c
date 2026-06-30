/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_paths.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"

#define CLAW_PATHS_MAX_LEN 32

static const char *TAG = "claw_paths";

static char s_root_dirs[CLAW_PATH_ROOT_MAX][CLAW_PATHS_MAX_LEN];

esp_err_t claw_paths_set(claw_path_root_t root, const char *path)
{
    ESP_RETURN_ON_FALSE((unsigned)root < CLAW_PATH_ROOT_MAX, ESP_ERR_INVALID_ARG, TAG, "bad root %d", (int)root);
    ESP_RETURN_ON_FALSE(path && path[0], ESP_ERR_INVALID_ARG, TAG, "empty path");
    ESP_RETURN_ON_FALSE(strlcpy(s_root_dirs[root], path, sizeof(s_root_dirs[root])) < sizeof(s_root_dirs[root]),
                        ESP_ERR_INVALID_SIZE, TAG, "root path too long: %s", path);
    return ESP_OK;
}

const char *claw_paths_get(claw_path_root_t root)
{
    if ((unsigned)root >= CLAW_PATH_ROOT_MAX || !s_root_dirs[root][0]) {
        return NULL;
    }
    return s_root_dirs[root];
}

esp_err_t claw_paths_join(claw_path_root_t root, const char *subpath, char *out, size_t out_size)
{
    const char *base = claw_paths_get(root);
    int written;

    ESP_RETURN_ON_FALSE(base, ESP_ERR_INVALID_STATE, TAG, "root %d not set", (int)root);
    ESP_RETURN_ON_FALSE(out && out_size, ESP_ERR_INVALID_ARG, TAG, "bad out buffer");

    if (subpath && subpath[0]) {
        written = snprintf(out, out_size, "%s/%s", base, subpath);
    } else {
        written = snprintf(out, out_size, "%s", base);
    }
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < out_size, ESP_ERR_INVALID_SIZE, TAG,
                        "joined path too long: %s/%s", base, subpath ? subpath : "");
    return ESP_OK;
}
