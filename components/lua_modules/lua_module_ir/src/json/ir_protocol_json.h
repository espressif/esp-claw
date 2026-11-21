/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "ir_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IR_PROTOCOL_ID_MAX 64
#define IR_PROTOCOL_FILE_MAX (64 * 1024)
#define IR_PROTOCOL_FILE_SUFFIX ".json"

struct cJSON;

/* Parses a JSON protocol: an inline, encode only description plus its capabilities. The result owns every table
 * and is released with ir_protocol_free(). */
esp_err_t ir_protocol_parse(const char *json, size_t length, ir_protocol_t **ret_protocol);
/* Same as ir_protocol_parse() for an already parsed object; root stays owned by the caller. */
esp_err_t ir_protocol_parse_object(const struct cJSON *root, ir_protocol_t **ret_protocol);
esp_err_t ir_protocol_load(const char *path, ir_protocol_t **ret_protocol);
/* Frees a protocol returned by parse, load or open; builtin protocols must not be passed. */
void ir_protocol_free(ir_protocol_t *protocol);

bool ir_protocol_id_valid(const char *id);

/* A source is a directory of <id>.json files, or a JSONL bundle file with one protocol per line, each line
 * starting with {"id":"<id>". Missing sources are skipped; earlier sources hide later ones. */
/* Loads id from the first source that has it; the protocol must declare the same id. */
esp_err_t ir_protocol_open(const char *const *sources, size_t source_count, const char *id, ir_protocol_t **ret_protocol);
/* Calls cb once per id with the loaded protocol, freed after cb returns. Entries that do not load, or declare
 * another id, are skipped. Return false from cb to stop. */
typedef bool (*ir_protocol_list_cb_t)(const ir_protocol_t *protocol, void *user_ctx);
esp_err_t ir_protocol_list(const char *const *sources, size_t source_count, ir_protocol_list_cb_t cb, void *user_ctx);

#ifdef __cplusplus
}
#endif
