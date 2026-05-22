/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_memory_internal.h"

#include <ctype.h>
#include <inttypes.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

void safe_copy(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    len = strnlen(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

char *dup_printf(const char *fmt, ...)
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

static void claw_memory_sanitize_session_id(const char *session_id, char *buf, size_t size)
{
    size_t off = 0;

    if (!buf || size == 0) {
        return;
    }
    buf[0] = '\0';
    if (!session_id) {
        return;
    }

    while (*session_id && off + 1 < size) {
        char ch = *session_id++;

        if (isalnum((unsigned char)ch) || ch == '-' || ch == '_') {
            buf[off++] = ch;
        } else {
            buf[off++] = '_';
        }
    }
    buf[off] = '\0';
}

size_t claw_memory_text_buffer_size(size_t max_chars)
{
    if (max_chars == 0 || max_chars > CLAW_MEMORY_DEFAULT_MAX_MESSAGE_CHARS) {
        max_chars = CLAW_MEMORY_DEFAULT_MAX_MESSAGE_CHARS;
    }
    return (max_chars * 4) + 1;
}

char *claw_memory_session_path_dup(const char *session_id)
{
    char safe_session_id[48];
    uint32_t hash = 2166136261u;
    const unsigned char *p = (const unsigned char *)session_id;
    size_t len;

    claw_memory_sanitize_session_id(session_id, safe_session_id, sizeof(safe_session_id));
    while (p && *p) {
        hash ^= *p++;
        hash *= 16777619u;
    }

    len = strnlen(safe_session_id, sizeof(safe_session_id) - 1);
    if (len > 24) {
        safe_session_id[24] = '\0';
    }

    return dup_printf("%s/s_%s_%08" PRIx32 ".log",
                      s_memory.session_root_dir,
                      safe_session_id[0] ? safe_session_id : "default",
                      hash);
}

void claw_memory_normalize_session_text(const char *src,
                                        char *dst,
                                        size_t dst_size,
                                        size_t max_chars)
{
    size_t off = 0;
    size_t chars = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    while (*src && off + 1 < dst_size && chars < max_chars) {
        const unsigned char *cur = (const unsigned char *)src;
        size_t seq_len = utf8_sequence_len(*cur);

        if (*cur < 0x80) {
            char ch = (char)*cur++;

            src = (const char *)cur;
            dst[off++] = ch;
            chars++;
            continue;
        }

        if (seq_len == 0 || !utf8_sequence_valid(cur, seq_len) || off + seq_len >= dst_size) {
            src++;
            continue;
        }
        memcpy(dst + off, cur, seq_len);
        off += seq_len;
        src += seq_len;
        chars++;
    }
    dst[off] = '\0';
}

claw_memory_backend_format_t claw_memory_backend_format_from_type(const char *backend_type)
{
    if (!backend_type || !backend_type[0]) {
        return CLAW_MEMORY_BACKEND_FORMAT_UNKNOWN;
    }
    if (strcmp(backend_type, "openai_compatible") == 0) {
        return CLAW_MEMORY_BACKEND_FORMAT_OPENAI;
    }
    if (strcmp(backend_type, "anthropic_compatible") == 0) {
        return CLAW_MEMORY_BACKEND_FORMAT_ANTHROPIC;
    }
    return CLAW_MEMORY_BACKEND_FORMAT_UNKNOWN;
}

esp_err_t claw_memory_write_session_raw_record(FILE *file,
                                               const char *json_text,
                                               uint32_t *out_offset,
                                               uint32_t *out_length)
{
    size_t record_len;
    long offset;

    if (!file || !json_text || !out_offset || !out_length) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_offset = 0;
    *out_length = 0;

    offset = ftell(file);
    if (offset < 0 || (uint64_t)offset > UINT32_MAX) {
        return ESP_FAIL;
    }

    record_len = strlen(json_text);
    if (record_len + 1 > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (fwrite(json_text, 1, record_len, file) != record_len ||
            fputc('\n', file) == EOF) {
        return ESP_FAIL;
    }

    *out_offset = (uint32_t)offset;
    *out_length = (uint32_t)(record_len + 1);
    return ESP_OK;
}

bool line_list_contains_item(const char *list, const char *item)
{
    const char *cursor = list;
    size_t item_len;

    if (!list || !item || !item[0]) {
        return false;
    }

    item_len = strlen(item);
    while (cursor && *cursor) {
        const char *line_end = strchr(cursor, '\n');
        size_t line_len = line_end ? (size_t)(line_end - cursor) : strlen(cursor);

        if (line_len == item_len && strncmp(cursor, item, item_len) == 0) {
            return true;
        }
        cursor = line_end ? line_end + 1 : NULL;
    }
    return false;
}

esp_err_t line_list_append_unique(char **list, const char *item)
{
    char *grown = NULL;
    size_t current_len;
    size_t item_len;

    if (!list || !item || !item[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (line_list_contains_item(*list, item)) {
        return ESP_OK;
    }

    item_len = strlen(item);
    if (!*list) {
        *list = dup_printf("%s", item);
        return *list ? ESP_OK : ESP_ERR_NO_MEM;
    }

    current_len = strlen(*list);
    grown = realloc(*list, current_len + 1 + item_len + 1);
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }

    *list = grown;
    (*list)[current_len] = '\n';
    memcpy((*list) + current_len + 1, item, item_len + 1);
    return ESP_OK;
}

esp_err_t line_list_merge_unique(char **dst, const char *src)
{
    const char *cursor = src;

    if (!dst || !src || !src[0]) {
        return ESP_OK;
    }

    while (cursor && *cursor) {
        const char *line_end = strchr(cursor, '\n');
        size_t line_len = line_end ? (size_t)(line_end - cursor) : strlen(cursor);
        char line[256] = {0};
        size_t copy_len = line_len < sizeof(line) - 1 ? line_len : sizeof(line) - 1;
        esp_err_t err;

        memcpy(line, cursor, copy_len);
        line[copy_len] = '\0';
        err = line_list_append_unique(dst, line);
        if (err != ESP_OK) {
            return err;
        }
        cursor = line_end ? line_end + 1 : NULL;
    }

    return ESP_OK;
}

char *claw_memory_format_update_stage_note(const char *summary_list)
{
    const char *cursor = summary_list;
    size_t total_len = strlen("Memory update: []");
    size_t count = 0;
    char *note = NULL;
    size_t off = 0;

    if (!summary_list || !summary_list[0]) {
        return NULL;
    }

    while (cursor && *cursor) {
        const char *line_end = strchr(cursor, '\n');
        size_t line_len = line_end ? (size_t)(line_end - cursor) : strlen(cursor);

        total_len += line_len;
        if (count > 0) {
            total_len += strlen(";");
        }
        count++;
        cursor = line_end ? line_end + 1 : NULL;
    }

    note = calloc(1, total_len + 1);
    if (!note) {
        return NULL;
    }

    off += snprintf(note + off, total_len + 1 - off, "Memory update: [");
    cursor = summary_list;
    count = 0;
    while (cursor && *cursor) {
        const char *line_end = strchr(cursor, '\n');
        size_t line_len = line_end ? (size_t)(line_end - cursor) : strlen(cursor);

        if (count > 0) {
            off += snprintf(note + off, total_len + 1 - off, ";");
        }
        off += snprintf(note + off, total_len + 1 - off, "%.*s", (int)line_len, cursor);
        count++;
        cursor = line_end ? line_end + 1 : NULL;
    }
    snprintf(note + off, total_len + 1 - off, "]");
    return note;
}

esp_err_t claw_memory_append_item_summary_labels(const claw_memory_item_t *item,
                                                 char **out_summary_list)
{
    char labels[CLAW_MEMORY_MAX_SUMMARIES][CLAW_MEMORY_MAX_LABEL_TEXT];
    size_t label_count = 0;
    size_t i;

    if (!item || !out_summary_list) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_memory_collect_summary_labels(item, labels, &label_count);
    for (i = 0; i < label_count; i++) {
        esp_err_t err = line_list_append_unique(out_summary_list, labels[i]);

        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

uint32_t claw_memory_now_sec(void)
{
    struct timeval tv = {0};

    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec;
}

size_t utf8_sequence_len(unsigned char ch)
{
    if (ch < 0x80) {
        return 1;
    }
    if ((ch & 0xE0) == 0xC0) {
        return 2;
    }
    if ((ch & 0xF0) == 0xE0) {
        return 3;
    }
    if ((ch & 0xF8) == 0xF0) {
        return 4;
    }
    return 0;
}

bool utf8_sequence_valid(const unsigned char *src, size_t len)
{
    size_t i;

    if (!src || len == 0) {
        return false;
    }
    for (i = 1; i < len; i++) {
        if ((src[i] & 0xC0) != 0x80) {
            return false;
        }
    }
    return true;
}

void utf8_copy_chars(char *dst, size_t dst_size, const char *src, size_t max_chars)
{
    size_t off = 0;
    size_t chars = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    while (*src && off + 1 < dst_size && chars < max_chars) {
        const unsigned char *cur = (const unsigned char *)src;
        size_t seq_len = utf8_sequence_len(*cur);

        if (seq_len == 0 || !utf8_sequence_valid(cur, seq_len)) {
            src++;
            continue;
        }
        if (off + seq_len >= dst_size) {
            break;
        }
        memcpy(dst + off, cur, seq_len);
        off += seq_len;
        src += seq_len;
        chars++;
    }
    dst[off] = '\0';
}

bool utf8_string_is_valid(const char *src)
{
    while (src && *src) {
        const unsigned char *cur = (const unsigned char *)src;
        size_t seq_len = utf8_sequence_len(*cur);

        if (*cur < 0x80) {
            src++;
            continue;
        }
        if (seq_len == 0 || !utf8_sequence_valid(cur, seq_len)) {
            return false;
        }
        src += seq_len;
    }
    return true;
}

void trim_whitespace(char *text)
{
    char *start = text;
    char *end;

    if (!text) {
        return;
    }

    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
}

bool text_contains_ascii_ci(const char *haystack, const char *needle)
{
    size_t needle_len;

    if (!haystack || !needle || !needle[0]) {
        return false;
    }
    needle_len = strlen(needle);
    while (*haystack) {
        size_t i;
        for (i = 0; i < needle_len; i++) {
            unsigned char a = (unsigned char)haystack[i];
            unsigned char b = (unsigned char)needle[i];

            if (!a) {
                return false;
            }
            if (tolower(a) != tolower(b)) {
                break;
            }
        }
        if (i == needle_len) {
            return true;
        }
        haystack++;
    }
    return false;
}

bool utf8_matches_literal(const unsigned char *src, size_t seq_len, const char *literal)
{
    return src && literal && strlen(literal) == seq_len && memcmp(src, literal, seq_len) == 0;
}

bool utf8_is_common_punctuation(const unsigned char *src, size_t seq_len)
{
    static const char *const puncts[] = {
        "。", "，", "！", "？", "；", "：", "、",
        "（", "）", "【", "】", "《", "》",
        "“", "”", "‘", "’", "「", "」", "『", "』",
        "·", "…", "—", "－", "～", "　", "．"
    };
    size_t i;

    for (i = 0; i < sizeof(puncts) / sizeof(puncts[0]); i++) {
        if (utf8_matches_literal(src, seq_len, puncts[i])) {
            return true;
        }
    }
    return false;
}

void normalize_text_for_key(const char *src, char *dst, size_t dst_size)
{
    size_t off = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    while (*src && off + 1 < dst_size) {
        const unsigned char *cur = (const unsigned char *)src;
        size_t seq_len = utf8_sequence_len(*cur);

        if (*cur < 0x80) {
            char ch = (char)tolower(*cur);

            src++;
            if (isalnum((unsigned char)ch)) {
                dst[off++] = ch;
            }
            continue;
        }

        if (seq_len == 0 || !utf8_sequence_valid(cur, seq_len)) {
            src++;
            continue;
        }
        if (utf8_is_common_punctuation(cur, seq_len)) {
            src += seq_len;
            continue;
        }
        if (off + seq_len >= dst_size) {
            break;
        }
        memcpy(dst + off, cur, seq_len);
        off += seq_len;
        src += seq_len;
    }
    dst[off] = '\0';
}

esp_err_t ensure_dir_recursive(const char *path)
{
    char tmp[CLAW_MEMORY_MAX_PATH];
    size_t i;
    size_t last_existing = 0;

    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(path) >= sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }

    safe_copy(tmp, sizeof(tmp), path);

    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            struct stat st = {0};

            tmp[i] = '\0';
            if (tmp[0] && stat(tmp, &st) == 0) {
                last_existing = i;
            }
            tmp[i] = '/';
        }
    }

    {
        struct stat st = {0};
        if (stat(tmp, &st) == 0) {
            return ESP_OK;
        }
    }

    {
        size_t start = last_existing ? last_existing + 1 : 1;
        for (i = start; tmp[i]; i++) {
            if (tmp[i] == '/') {
                struct stat st = {0};

                tmp[i] = '\0';
                if (tmp[0] && stat(tmp, &st) != 0) {
                    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                        tmp[i] = '/';
                        return ESP_FAIL;
                    }
                }
                tmp[i] = '/';
            }
        }
    }

    {
        struct stat st = {0};
        if (stat(tmp, &st) != 0) {
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return ESP_FAIL;
            }
        }
    }

    return ESP_OK;
}

esp_err_t ensure_parent_dir(const char *path)
{
    char tmp[CLAW_MEMORY_MAX_PATH];
    char *slash;

    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(path) >= sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }

    safe_copy(tmp, sizeof(tmp), path);
    slash = strrchr(tmp, '/');
    if (!slash) {
        return ESP_OK;
    }
    *slash = '\0';
    if (!tmp[0]) {
        return ESP_OK;
    }
    return ensure_dir_recursive(tmp);
}

esp_err_t read_file_dup(const char *path, char **out_buf)
{
    FILE *file = NULL;
    long size = 0;
    char *buf = NULL;
    size_t read_bytes;

    if (!path || !out_buf) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;

    file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    buf = calloc(1, (size_t)size + 1);
    if (!buf) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    read_bytes = fread(buf, 1, (size_t)size, file);
    fclose(file);
    buf[read_bytes] = '\0';
    *out_buf = buf;
    return ESP_OK;
}

esp_err_t write_file_text(const char *path, const char *text)
{
    FILE *file = NULL;

    if (!path || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ensure_parent_dir(path) != ESP_OK) {
        return ESP_FAIL;
    }

    file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }
    if (fputs(text, file) < 0) {
        fclose(file);
        return ESP_FAIL;
    }
    fclose(file);
    return ESP_OK;
}

esp_err_t append_file_text(const char *path, const char *text)
{
    FILE *file = NULL;

    if (!path || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ensure_parent_dir(path) != ESP_OK) {
        return ESP_FAIL;
    }

    file = fopen(path, "ab");
    if (!file) {
        return ESP_FAIL;
    }
    if (fputs(text, file) < 0) {
        fclose(file);
        return ESP_FAIL;
    }
    fclose(file);
    return ESP_OK;
}

size_t file_size_bytes(const char *path)
{
    struct stat st = {0};

    if (!path || stat(path, &st) != 0) {
        return 0;
    }
    return (size_t)st.st_size;
}

esp_err_t ensure_file_with_default(const char *path, const char *default_text)
{
    FILE *file;

    if (!path || !default_text) {
        return ESP_ERR_INVALID_ARG;
    }

    file = fopen(path, "rb");
    if (file) {
        fclose(file);
        return ESP_OK;
    }
    return write_file_text(path, default_text);
}

esp_err_t claw_memory_join_path(char *dst,
                                size_t dst_size,
                                const char *dir,
                                const char *name)
{
    size_t dir_len;
    size_t name_len;

    if (!dst || dst_size == 0 || !dir || !name) {
        return ESP_ERR_INVALID_ARG;
    }

    dir_len = strlen(dir);
    name_len = strlen(name);
    if (dir_len + 1 + name_len + 1 > dst_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(dst, dir, dir_len);
    dst[dir_len] = '/';
    memcpy(dst + dir_len + 1, name, name_len);
    dst[dir_len + 1 + name_len] = '\0';
    return ESP_OK;
}
