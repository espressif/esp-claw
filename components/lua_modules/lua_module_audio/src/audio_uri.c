/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_private.h"

static bool audio_has_scheme(const char *s)
{
    return s && strstr(s, "://") != NULL;
}

bool audio_path_valid(const char *path, const char *ext)
{
    size_t ext_len = strlen(ext);
    size_t len;

    if (!path || !path[0] || strstr(path, "..")) {
        return false;
    }
    len = strlen(path);
    return len > ext_len && strcmp(path + len - ext_len, ext) == 0;
}

char *audio_uri_from_path(const char *path)
{
    if (audio_has_scheme(path)) {
        return audio_strdup(path);
    }
    if (!path || path[0] != '/') {
        return audio_strdup(path ? path : "");
    }

    size_t len = strlen("file://") + strlen(path) + 1;
    char *uri = malloc(len);
    if (!uri) {
        return NULL;
    }
    memcpy(uri, "file://", strlen("file://"));
    memcpy(uri + strlen("file://"), path, strlen(path) + 1);
    return uri;
}

char *audio_path_from_file_arg(const char *path_or_uri)
{
    const char *prefix = "file://";
    if (!path_or_uri) {
        return NULL;
    }
    if (strncmp(path_or_uri, prefix, strlen(prefix)) != 0) {
        return audio_strdup(path_or_uri);
    }

    const char *host = path_or_uri + strlen(prefix);
    const char *tail = strchr(host, '/');
    if (!tail || tail == host) {
        return NULL;
    }

    size_t host_len = (size_t)(tail - host);
    size_t tail_len = strlen(tail);
    char *path = malloc(1 + host_len + tail_len + 1);
    if (!path) {
        return NULL;
    }
    path[0] = '/';
    memcpy(path + 1, host, host_len);
    memcpy(path + 1 + host_len, tail, tail_len + 1);
    return path;
}
