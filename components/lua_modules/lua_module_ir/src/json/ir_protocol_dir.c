/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ir_protocol_json.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

#define PATH_SIZE 256
#define LINE_CHUNK 1024
#define BUNDLE_PREFIX "{\"id\":\""

static const char *TAG = "ir_protocol_dir";

typedef enum {
    SOURCE_NONE,
    SOURCE_DIR,
    SOURCE_BUNDLE,
} source_kind_t;

/* Called for every bundle line with a valid leading id; return false to stop the scan. */
typedef bool (*bundle_visit_t)(const char *id, const char *line, size_t length, void *ctx);

typedef struct {
    FILE *file;
    char *text;
    size_t size;                        /* allocated bytes */
    size_t length;                      /* current line, newline removed */
} line_reader_t;

typedef struct {
    const char *id;
    ir_protocol_t **ret_protocol;       /* NULL only checks presence */
    esp_err_t err;
} bundle_find_t;

typedef struct {
    const char *const *sources;
    size_t index;
    char *path;
    ir_protocol_list_cb_t cb;
    void *user_ctx;
    bool more;
} list_ctx_t;

bool ir_protocol_id_valid(const char *id)
{
    size_t length = id ? strlen(id) : 0;
    if (!length || length >= IR_PROTOCOL_ID_MAX || id[0] == '.') return false;
    for (const char *c = id; *c; ++c) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '.' || *c == '-')) return false;
    }
    return true;
}

static bool protocol_path(const char *dir, const char *id, char *path)
{
    int written = snprintf(path, PATH_SIZE, "%s/%s" IR_PROTOCOL_FILE_SUFFIX, dir, id);
    return written > 0 && written < PATH_SIZE;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static source_kind_t source_kind(const char *source)
{
    struct stat st;
    if (!source || stat(source, &st) != 0) return SOURCE_NONE;
    if (S_ISDIR(st.st_mode)) return SOURCE_DIR;
    return S_ISREG(st.st_mode) ? SOURCE_BUNDLE : SOURCE_NONE;
}

/* Reads the next line into reader->text; ESP_ERR_NOT_FOUND at the end of the file. */
static esp_err_t line_next(line_reader_t *r)
{
    r->length = 0;
    for (;;) {
        if (r->size - r->length < LINE_CHUNK) {
            size_t size = r->size ? r->size * 2 : LINE_CHUNK;
            char *text = realloc(r->text, size);
            if (!text) return ESP_ERR_NO_MEM;
            r->text = text;
            r->size = size;
        }
        if (!fgets(r->text + r->length, (int)(r->size - r->length), r->file)) {
            if (ferror(r->file)) return ESP_FAIL;
            return r->length ? ESP_OK : ESP_ERR_NOT_FOUND;
        }
        r->length += strlen(r->text + r->length);
        if (r->length > IR_PROTOCOL_FILE_MAX) return ESP_ERR_INVALID_SIZE;
        if (r->text[r->length - 1] == '\n') {
            r->text[--r->length] = 0;
            return ESP_OK;
        }
    }
}

/* Copies the leading id of a bundle line; false when the line does not start with {"id":"<valid id>". */
static bool line_id(const char *line, char *id)
{
    const size_t prefix = sizeof(BUNDLE_PREFIX) - 1;
    if (strncmp(line, BUNDLE_PREFIX, prefix)) return false;
    const char *start = line + prefix;
    const char *end = strchr(start, '"');
    if (!end || (size_t)(end - start) >= IR_PROTOCOL_ID_MAX) return false;
    memcpy(id, start, (size_t)(end - start));
    id[end - start] = 0;
    return ir_protocol_id_valid(id);
}

static esp_err_t bundle_scan(const char *path, bundle_visit_t visit, void *ctx)
{
    line_reader_t r = {.file = fopen(path, "rb")};
    if (!r.file) return ESP_ERR_NOT_FOUND;
    char id[IR_PROTOCOL_ID_MAX];
    esp_err_t err;
    while ((err = line_next(&r)) == ESP_OK) {
        if (!r.length) continue;
        if (!line_id(r.text, id)) {
            ESP_LOGW(TAG, "%s: line without a leading id skipped", path);
            continue;
        }
        if (!visit(id, r.text, r.length, ctx)) break;
    }
    fclose(r.file);
    free(r.text);
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) ESP_LOGW(TAG, "read %s failed: %s", path, esp_err_to_name(err));
    return err;
}

static esp_err_t bundle_parse(const char *id, const char *line, size_t length, ir_protocol_t **ret_protocol)
{
    esp_err_t err = ir_protocol_parse(line, length, ret_protocol);
    if (err != ESP_OK) ESP_LOGW(TAG, "bundle protocol %s invalid: %s", id, esp_err_to_name(err));
    return err;
}

static bool bundle_find_visit(const char *id, const char *line, size_t length, void *ctx)
{
    bundle_find_t *find = ctx;
    if (strcmp(id, find->id)) return true;
    find->err = find->ret_protocol ? bundle_parse(id, line, length, find->ret_protocol) : ESP_OK;
    return false;
}

/* Looks id up in one source, ESP_ERR_NOT_FOUND when absent. ret_protocol NULL only checks presence. */
static esp_err_t source_find(const char *source, const char *id, char *path, ir_protocol_t **ret_protocol)
{
    switch (source_kind(source)) {
    case SOURCE_DIR:
        if (!protocol_path(source, id, path) || !file_exists(path)) return ESP_ERR_NOT_FOUND;
        return ret_protocol ? ir_protocol_load(path, ret_protocol) : ESP_OK;
    case SOURCE_BUNDLE: {
        bundle_find_t find = {.id = id, .ret_protocol = ret_protocol, .err = ESP_ERR_NOT_FOUND};
        esp_err_t err = bundle_scan(source, bundle_find_visit, &find);
        return err != ESP_OK ? err : find.err;
    }
    default:
        return ESP_ERR_NOT_FOUND;
    }
}

esp_err_t ir_protocol_open(const char *const *sources, size_t source_count, const char *id, ir_protocol_t **ret_protocol)
{
    if (!sources || !ret_protocol || !ir_protocol_id_valid(id)) return ESP_ERR_INVALID_ARG;
    *ret_protocol = NULL;
    char *path = malloc(PATH_SIZE);
    if (!path) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; err == ESP_ERR_NOT_FOUND && i < source_count; ++i) err = source_find(sources[i], id, path, ret_protocol);
    free(path);
    if (err == ESP_OK && strcmp((*ret_protocol)->id, id)) {
        ir_protocol_free(*ret_protocol);
        *ret_protocol = NULL;
        err = ESP_ERR_INVALID_ARG;
    }
    return err;
}

/* Returns the id of a protocol file name in id, or false when the name is not a protocol file. */
static bool file_id(const char *name, char *id)
{
    size_t length = strlen(name);
    size_t suffix = strlen(IR_PROTOCOL_FILE_SUFFIX);
    if (length <= suffix || length - suffix >= IR_PROTOCOL_ID_MAX || strcmp(name + length - suffix, IR_PROTOCOL_FILE_SUFFIX)) return false;
    memcpy(id, name, length - suffix);
    id[length - suffix] = 0;
    return ir_protocol_id_valid(id);
}

static bool hidden_by_earlier(list_ctx_t *ctx, const char *id)
{
    for (size_t j = 0; j < ctx->index; ++j) {
        if (source_find(ctx->sources[j], id, ctx->path, NULL) != ESP_ERR_NOT_FOUND) return true;
    }
    return false;
}

static void list_emit(list_ctx_t *ctx, const char *id, esp_err_t err, ir_protocol_t *protocol)
{
    if (err == ESP_OK && !strcmp(protocol->id, id)) ctx->more = ctx->cb(protocol, ctx->user_ctx);
    ir_protocol_free(protocol);
}

static bool list_bundle_visit(const char *id, const char *line, size_t length, void *user_ctx)
{
    list_ctx_t *ctx = user_ctx;
    if (!hidden_by_earlier(ctx, id)) {
        ir_protocol_t *protocol = NULL;
        esp_err_t err = bundle_parse(id, line, length, &protocol);
        list_emit(ctx, id, err, protocol);
    }
    return ctx->more;
}

static void list_dir(list_ctx_t *ctx, const char *source, char *id)
{
    DIR *dir = opendir(source);
    struct dirent *entry;
    while (dir && ctx->more && (entry = readdir(dir)) != NULL) {
        if (!file_id(entry->d_name, id) || hidden_by_earlier(ctx, id) || !protocol_path(source, id, ctx->path)) continue;
        ir_protocol_t *protocol = NULL;
        esp_err_t err = ir_protocol_load(ctx->path, &protocol);
        list_emit(ctx, id, err, protocol);
    }
    if (dir) closedir(dir);
}

esp_err_t ir_protocol_list(const char *const *sources, size_t source_count, ir_protocol_list_cb_t cb, void *user_ctx)
{
    if (!sources || !cb) return ESP_ERR_INVALID_ARG;
    char *path = malloc(PATH_SIZE + IR_PROTOCOL_ID_MAX);
    if (!path) return ESP_ERR_NO_MEM;
    list_ctx_t ctx = {.sources = sources, .path = path, .cb = cb, .user_ctx = user_ctx, .more = true};
    esp_err_t err = ESP_OK;
    for (; err == ESP_OK && ctx.more && ctx.index < source_count; ++ctx.index) {
        switch (source_kind(sources[ctx.index])) {
        case SOURCE_DIR:
            list_dir(&ctx, sources[ctx.index], path + PATH_SIZE);
            break;
        case SOURCE_BUNDLE:
            err = bundle_scan(sources[ctx.index], list_bundle_visit, &ctx);
            break;
        default:
            break;
        }
    }
    free(path);
    return err;
}
