/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/media/claw_media_pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "mbedtls/base64.h"

static char *dup_printf(const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;
    char *buf;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    buf = calloc(1, (size_t)needed + 1);
    if (!buf) {
        va_end(args);
        return NULL;
    }

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
}

static const char *image_mime_from_path(const char *path)
{
    const char *dot;

    if (!path || !path[0]) {
        return NULL;
    }

    dot = strrchr(path, '.');
    if (!dot) {
        return NULL;
    }

    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(dot, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(dot, ".webp") == 0) {
        return "image/webp";
    }

    return NULL;
}

static esp_err_t prepare_local_path_asset(const claw_media_asset_t *asset,
                                          const claw_llm_model_profile_t *profile,
                                          size_t image_max_bytes,
                                          claw_media_prepared_t *out_prepared,
                                          char **out_error_message)
{
    struct stat st = {0};
    FILE *file = NULL;
    unsigned char *raw = NULL;
    unsigned char *encoded = NULL;
    size_t encoded_len = 0;
    size_t prefix_len;
    size_t read_len;
    const char *mime;
    char *data_url = NULL;

    if (!asset->path || !asset->path[0]) {
        *out_error_message = dup_printf("media path is empty");
        return ESP_ERR_INVALID_ARG;
    }
    if (asset->path[0] != '/') {
        *out_error_message = dup_printf("media path must be an absolute path");
        return ESP_ERR_INVALID_ARG;
    }

    mime = asset->mime_type ? asset->mime_type : image_mime_from_path(asset->path);
    if (!mime) {
        *out_error_message = dup_printf("Only local jpg/jpeg/png/gif/webp files are supported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (stat(asset->path, &st) != 0) {
        *out_error_message = dup_printf("Media file not found: %s", asset->path);
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0) {
        *out_error_message = dup_printf("Media file is empty: %s", asset->path);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((size_t)st.st_size > image_max_bytes) {
        *out_error_message = dup_printf("Media file is too large (%ld bytes > %u bytes)",
                                        (long)st.st_size,
                                        (unsigned)image_max_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    file = fopen(asset->path, "rb");
    if (!file) {
        *out_error_message = dup_printf("Failed to open media file: %s", asset->path);
        return ESP_FAIL;
    }

    raw = malloc((size_t)st.st_size);
    if (!raw) {
        *out_error_message = dup_printf("Out of memory reading media");
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    read_len = fread(raw, 1, (size_t)st.st_size, file);
    fclose(file);
    if (read_len != (size_t)st.st_size) {
        free(raw);
        *out_error_message = dup_printf("Failed to read full media file: %s", asset->path);
        return ESP_FAIL;
    }

    encoded_len = ((size_t)st.st_size + 2) / 3 * 4;
    encoded = calloc(1, encoded_len + 1);
    if (!encoded) {
        free(raw);
        *out_error_message = dup_printf("Out of memory encoding media");
        return ESP_ERR_NO_MEM;
    }

    if (mbedtls_base64_encode(encoded, encoded_len + 1, &encoded_len, raw, (size_t)st.st_size) != 0) {
        free(raw);
        free(encoded);
        *out_error_message = dup_printf("Failed to base64-encode media");
        return ESP_FAIL;
    }

    prefix_len = strlen("data:") + strlen(mime) + strlen(";base64,");
    data_url = calloc(1, prefix_len + encoded_len + 1);
    if (!data_url) {
        free(raw);
        free(encoded);
        *out_error_message = dup_printf("Out of memory building media payload");
        return ESP_ERR_NO_MEM;
    }

    snprintf(data_url, prefix_len + 1, "data:%s;base64,", mime);
    memcpy(data_url + prefix_len, encoded, encoded_len);
    data_url[prefix_len + encoded_len] = '\0';

    out_prepared->kind = CLAW_MEDIA_PREPARED_KIND_DATA_URL;
    out_prepared->payload = data_url;
    out_prepared->original_size = (size_t)st.st_size;
    strlcpy(out_prepared->mime_type, mime, sizeof(out_prepared->mime_type));

    free(raw);
    free(encoded);
    (void)profile;
    return ESP_OK;
}

esp_err_t claw_media_prepare_asset(const claw_media_asset_t *asset,
                                   const claw_llm_model_profile_t *profile,
                                   size_t image_max_bytes,
                                   claw_media_prepared_t *out_prepared,
                                   char **out_error_message)
{
    if (out_prepared) {
        memset(out_prepared, 0, sizeof(*out_prepared));
    }
    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!asset || !profile || !out_prepared || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    if (asset->kind == CLAW_MEDIA_ASSET_KIND_REMOTE_URL) {
        if (!asset->url || !asset->url[0]) {
            *out_error_message = dup_printf("media url is empty");
            return ESP_ERR_INVALID_ARG;
        }
        out_prepared->kind = CLAW_MEDIA_PREPARED_KIND_REMOTE_URL;
        out_prepared->payload = strdup(asset->url);
        if (!out_prepared->payload) {
            *out_error_message = dup_printf("Out of memory copying media URL");
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    if (asset->kind != CLAW_MEDIA_ASSET_KIND_LOCAL_PATH) {
        *out_error_message = dup_printf("Unsupported media asset kind");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (profile->image_remote_url_only) {
        *out_error_message = dup_printf("Selected profile only supports remote image URLs");
        return ESP_ERR_NOT_SUPPORTED;
    }

    return prepare_local_path_asset(asset, profile, image_max_bytes, out_prepared, out_error_message);
}

void claw_media_prepared_free(claw_media_prepared_t *prepared)
{
    if (!prepared) {
        return;
    }

    free(prepared->payload);
    memset(prepared, 0, sizeof(*prepared));
}
