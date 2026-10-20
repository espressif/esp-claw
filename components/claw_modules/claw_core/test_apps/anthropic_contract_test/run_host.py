#!/usr/bin/env python3
"""Run offline requests through the production config, backend and HTTP transport.

Pass --cjson-dir pointing to cJSON sources and --build-dir for host artifacts.
Only the ESP-IDF HTTP client and platform services are replaced by test doubles.
"""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cjson-dir', type=Path, required=True)
parser.add_argument('--build-dir', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[5]
core = root / 'components/claw_modules/claw_core'
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
stubs = {
    'esp_err.h': '''#pragma once
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_INVALID_RESPONSE 0x108
const char *esp_err_to_name(esp_err_t err);
size_t strlcpy(char *dst, const char *src, size_t size);
''',
    'esp_check.h': '#pragma once\n',
    'esp_log.h': '''#pragma once
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
''',
    'claw_core.h': '''#pragma once
typedef void *claw_core_handle_t;
typedef struct claw_core_request claw_core_request_t;
typedef struct claw_core_context_provider claw_core_context_provider_t;
''',
    'esp_crt_bundle.h': '''#pragma once
#include "esp_err.h"
esp_err_t esp_crt_bundle_attach(void *conf);
''',
    'esp_http_client.h': '''#pragma once
#include "esp_err.h"
#define HTTP_EVENT_ON_DATA 1
#define HTTP_METHOD_POST 1
typedef struct {
    void *user_data;
    int event_id;
    void *data;
    int data_len;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    int timeout_ms, buffer_size, buffer_size_tx;
    esp_err_t (*crt_bundle_attach)(void *);
} esp_http_client_config_t;
typedef struct mock_client *esp_http_client_handle_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, int method);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *body, int len);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
''',
}
for name, content in stubs.items():
    (build / name).write_text(content)
exe = build / 'anthropic_contract_test'
subprocess.run([
    'cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
    '-I' + str(build), '-I' + str(args.cjson_dir.resolve()),
    '-I' + str(core / 'src'),
    '-I' + str(root / 'components/claw_capabilities/cap_llm_config/include'),
    '-I' + str(root / 'components/claw_modules/claw_cap/include'),
    str(Path(__file__).with_name('test_contract.c')),
    str(core / 'src/llm/backends/claw_llm_backend_anthropic.c'),
    str(core / 'src/llm/claw_llm_http_transport.c'),
    str(root / 'components/claw_capabilities/cap_llm_config/src/cap_llm_config.c'),
    str(args.cjson_dir.resolve() / 'cJSON.c'), '-o', str(exe),
], check=True)
subprocess.run([str(exe)], check=True)
