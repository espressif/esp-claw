/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_utils_file.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"

static const char *TAG = "claw_utils_file";

static void cleanup_temp_file(const char *path)
{
    if (remove(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "Failed to remove temp file %s errno=%d", path, errno);
    }
}

esp_err_t claw_utils_file_write_atomic(const char *path, const void *data, size_t size)
{
    static const char temp_suffix[] = ".tmp";
    FILE *file = NULL;
    char *temp_path = NULL;
    size_t path_len;
    esp_err_t err = ESP_FAIL;

    if (!path || !path[0] || (!data && size > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    path_len = strlen(path);
    if (path_len > SIZE_MAX - sizeof(temp_suffix)) {
        return ESP_ERR_INVALID_SIZE;
    }
    temp_path = malloc(path_len + sizeof(temp_suffix));
    if (!temp_path) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(temp_path, path, path_len);
    memcpy(temp_path + path_len, temp_suffix, sizeof(temp_suffix));

    file = fopen(temp_path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open temp file %s errno=%d", temp_path, errno);
        goto done;
    }

    if (size > 0 && fwrite(data, 1, size, file) != size) {
        ESP_LOGE(TAG, "Failed to write temp file %s errno=%d", temp_path, errno);
        goto close_file;
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        ESP_LOGE(TAG, "Failed to sync temp file %s errno=%d", temp_path, errno);
        goto close_file;
    }
    if (fclose(file) != 0) {
        file = NULL;
        ESP_LOGE(TAG, "Failed to close temp file %s errno=%d", temp_path, errno);
        goto cleanup;
    }
    file = NULL;

    /* Prefer atomic replacement; some FAT backends require removing the target first. */
    if (rename(temp_path, path) != 0) {
        int rename_errno = errno;
        if (rename_errno == EEXIST || rename_errno == ENOTEMPTY) {
            if ((remove(path) == 0 || errno == ENOENT) && rename(temp_path, path) == 0) {
                err = ESP_OK;
                goto done;
            }
            rename_errno = errno;
        }
        ESP_LOGE(TAG, "Failed to replace %s errno=%d", path, rename_errno);
        goto cleanup;
    }
    err = ESP_OK;
    goto done;

close_file:
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "Failed to close temp file %s errno=%d", temp_path, errno);
    }
    file = NULL;
cleanup:
    cleanup_temp_file(temp_path);
done:
    free(temp_path);
    return err;
}
