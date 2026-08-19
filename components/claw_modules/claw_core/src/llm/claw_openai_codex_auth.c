/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "claw_openai_codex_auth.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "cJSON.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "llm/claw_llm_http_transport.h"
#include "llm/claw_openai_codex_store.h"
#include "mbedtls/base64.h"

#define OPENAI_CODEX_CLIENT_ID \
    "app_EMoamEEZ73f0CkXaXp7hrann"

#define OPENAI_CODEX_DEVICE_CODE_URL \
    "https://auth.openai.com/api/accounts/deviceauth/usercode"

#define OPENAI_CODEX_DEVICE_TOKEN_URL \
    "https://auth.openai.com/api/accounts/deviceauth/token"

#define OPENAI_CODEX_TOKEN_URL \
    "https://auth.openai.com/oauth/token"

#define OPENAI_CODEX_VERIFICATION_URL \
    "https://auth.openai.com/codex/device"

#define OPENAI_CODEX_REDIRECT_URI \
    "https://auth.openai.com/deviceauth/callback"

#define OPENAI_CODEX_HTTP_TIMEOUT_MS 20000
#define OPENAI_CODEX_MAX_LOGIN_SECONDS (15 * 60)
#define OPENAI_CODEX_DEFAULT_INTERVAL 5
#define OPENAI_CODEX_POLL_TASK_STACK 8192
#define OPENAI_CODEX_POLL_TASK_PRIORITY 5

typedef struct {
    bool active;
    bool completed;

    char status[32];
    char message[160];

    char user_code[32];
    char verification_url[160];

    uint32_t interval;
} openai_codex_login_state_t;

typedef struct {
    char device_auth_id[256];
    char user_code[32];
    uint32_t interval;
} openai_codex_poll_args_t;

static const char *TAG = "openai_codex_auth";

static openai_codex_login_state_t s_login;

static SemaphoreHandle_t s_state_lock;
static bool s_worker_running;
static volatile bool s_cancel_requested;

/*
 * Tokens stay in RAM in this stage.
 * Persistent refresh-token storage will be added after this login path
 * has been verified end-to-end.
 */
static char *s_id_token;
static char *s_access_token;
static char *s_refresh_token;


static esp_err_t ensure_state_lock(void)
{
    if (s_state_lock) {
        return ESP_OK;
    }

    s_state_lock = xSemaphoreCreateMutex();

    if (!s_state_lock) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}


static void state_lock(void)
{
    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
}


static void state_unlock(void)
{
    if (s_state_lock) {
        xSemaphoreGive(s_state_lock);
    }
}


static void state_set_error(const char *message)
{
    state_lock();

    s_login.active = false;
    s_login.completed = false;

    strlcpy(s_login.status,
            "error",
            sizeof(s_login.status));

    strlcpy(s_login.message,
            message ? message : "OpenAI login error",
            sizeof(s_login.message));

    state_unlock();
}


static void worker_finished(void)
{
    state_lock();
    s_worker_running = false;
    state_unlock();
}


static char *string_printf(const char *fmt, ...)
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


static bool url_unreserved(unsigned char c)
{
    return isalnum(c) ||
           c == '-' ||
           c == '.' ||
           c == '_' ||
           c == '~';
}


static char *form_url_encode(const char *src)
{
    static const char hex[] = "0123456789ABCDEF";

    size_t src_len;
    size_t out_len = 0;
    char *out;
    char *dst;

    if (!src) {
        return NULL;
    }

    src_len = strlen(src);

    for (size_t i = 0; i < src_len; ++i) {
        unsigned char c = (unsigned char)src[i];

        out_len += url_unreserved(c) ? 1 : 3;
    }

    out = calloc(1, out_len + 1);

    if (!out) {
        return NULL;
    }

    dst = out;

    for (size_t i = 0; i < src_len; ++i) {
        unsigned char c = (unsigned char)src[i];

        if (url_unreserved(c)) {
            *dst++ = (char)c;
        } else {
            *dst++ = '%';
            *dst++ = hex[(c >> 4) & 0x0F];
            *dst++ = hex[c & 0x0F];
        }
    }

    *dst = '\0';

    return out;
}


static uint32_t parse_interval(cJSON *item)
{
    if (cJSON_IsNumber(item) && item->valuedouble > 0) {
        return (uint32_t)item->valuedouble;
    }

    if (cJSON_IsString(item) &&
            item->valuestring &&
            item->valuestring[0]) {

        char *end = NULL;

        unsigned long value =
            strtoul(item->valuestring, &end, 10);

        if (end &&
                *end == '\0' &&
                value > 0 &&
                value <= 300) {
            return (uint32_t)value;
        }
    }

    return OPENAI_CODEX_DEFAULT_INTERVAL;
}


static char *extract_chatgpt_account_id(const char *jwt)
{
    if (!jwt || !jwt[0]) return NULL;

    const char *dot1 = strchr(jwt, '.');
    if (!dot1) return NULL;
    const char *payload = dot1 + 1;
    const char *dot2 = strchr(payload, '.');
    if (!dot2 || dot2 == payload) return NULL;

    size_t payload_len = (size_t)(dot2 - payload);
    size_t padded_len = (payload_len + 3U) & ~3U;
    char *encoded = calloc(1, padded_len + 1U);
    if (!encoded) return NULL;

    for (size_t i = 0; i < payload_len; ++i) {
        char c = payload[i];
        encoded[i] = (c == '-') ? '+' : ((c == '_') ? '/' : c);
    }
    for (size_t i = payload_len; i < padded_len; ++i) encoded[i] = '=';

    size_t decoded_cap = (padded_len / 4U) * 3U + 1U;
    unsigned char *decoded = calloc(1, decoded_cap);
    if (!decoded) { free(encoded); return NULL; }

    size_t decoded_len = 0;
    int rc = mbedtls_base64_decode(decoded, decoded_cap - 1U, &decoded_len,
                                   (const unsigned char *)encoded, padded_len);
    free(encoded);
    if (rc != 0) { free(decoded); return NULL; }
    decoded[decoded_len] = '\0';

    cJSON *root = cJSON_Parse((const char *)decoded);
    free(decoded);
    if (!root) return NULL;

    cJSON *auth = cJSON_GetObjectItemCaseSensitive(root, "https://api.openai.com/auth");
    const char *account_id = NULL;
    if (cJSON_IsObject(auth)) {
        account_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(auth, "chatgpt_account_id"));
    }
    char *result = (account_id && account_id[0]) ? strdup(account_id) : NULL;
    cJSON_Delete(root);
    return result;
}
static esp_err_t exchange_authorization_code(
    const char *authorization_code,
    const char *code_verifier)
{
    char *encoded_code = NULL;
    char *encoded_redirect = NULL;
    char *encoded_client = NULL;
    char *encoded_verifier = NULL;
    char *body = NULL;

    claw_llm_http_json_request_t request = {0};
    claw_llm_http_response_t response = {0};

    char *transport_error = NULL;

    cJSON *root = NULL;

    const char *id_token_value;
    const char *access_token_value;
    const char *refresh_token_value;

    char *new_id_token = NULL;
    char *new_access_token = NULL;
    char *new_refresh_token = NULL;

    esp_err_t err = ESP_FAIL;

    encoded_code =
        form_url_encode(authorization_code);

    encoded_redirect =
        form_url_encode(OPENAI_CODEX_REDIRECT_URI);

    encoded_client =
        form_url_encode(OPENAI_CODEX_CLIENT_ID);

    encoded_verifier =
        form_url_encode(code_verifier);

    if (!encoded_code ||
            !encoded_redirect ||
            !encoded_client ||
            !encoded_verifier) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    body = string_printf(
        "grant_type=authorization_code"
        "&code=%s"
        "&redirect_uri=%s"
        "&client_id=%s"
        "&code_verifier=%s",
        encoded_code,
        encoded_redirect,
        encoded_client,
        encoded_verifier);

    if (!body) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    request.url = OPENAI_CODEX_TOKEN_URL;
    request.body = body;
    request.content_type =
        "application/x-www-form-urlencoded";
    request.accept_non_200 = true;
    request.timeout_ms =
        OPENAI_CODEX_HTTP_TIMEOUT_MS;

    err = claw_llm_http_post_json(
        &request,
        &response,
        &transport_error);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "OAuth token request failed: %s",
                 transport_error
                    ? transport_error
                    : esp_err_to_name(err));

        goto cleanup;
    }

    if (response.status_code < 200 ||
            response.status_code >= 300) {

        ESP_LOGE(TAG,
                 "OAuth token endpoint returned HTTP %d",
                 response.status_code);

        err = ESP_FAIL;
        goto cleanup;
    }

    if (!response.body || !response.body[0]) {
        ESP_LOGE(TAG,
                 "OAuth token endpoint returned empty body");

        err = ESP_FAIL;
        goto cleanup;
    }

    root = cJSON_Parse(response.body);

    if (!root) {
        ESP_LOGE(TAG,
                 "Could not parse OAuth token response");

        err = ESP_FAIL;
        goto cleanup;
    }

    id_token_value =
        cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(
                root,
                "id_token"));

    access_token_value =
        cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(
                root,
                "access_token"));

    refresh_token_value =
        cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(
                root,
                "refresh_token"));

    if (!id_token_value ||
            !id_token_value[0] ||
            !access_token_value ||
            !access_token_value[0] ||
            !refresh_token_value ||
            !refresh_token_value[0]) {

        ESP_LOGE(TAG,
                 "OAuth token response is incomplete");

        err = ESP_FAIL;
        goto cleanup;
    }

    new_id_token =
        strdup(id_token_value);

    new_access_token =
        strdup(access_token_value);

    new_refresh_token =
        strdup(refresh_token_value);

    if (!new_id_token ||
            !new_access_token ||
            !new_refresh_token) {

        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /* Persist only the durable credential. Access and ID tokens remain RAM-only. */
    err = claw_openai_codex_store_save_refresh_token(new_refresh_token);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Could not persist ChatGPT refresh token: %s",
                 esp_err_to_name(err));
        goto cleanup;
    }

    {
        char *account_id = extract_chatgpt_account_id(new_id_token);
        if (!account_id) account_id = extract_chatgpt_account_id(new_access_token);
        if (!account_id) {
            ESP_LOGE(TAG, "ChatGPT token does not contain chatgpt_account_id");
            err = ESP_FAIL;
            goto cleanup;
        }
        err = claw_openai_codex_store_save_account_id(account_id);
        free(account_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not persist ChatGPT account id: %s", esp_err_to_name(err));
            goto cleanup;
        }
    }
    /*
     * Swap credentials only after the complete response has
     * successfully been parsed.
     */
    state_lock();

    free(s_id_token);
    free(s_access_token);
    free(s_refresh_token);

    s_id_token = new_id_token;
    s_access_token = new_access_token;
    s_refresh_token = new_refresh_token;

    new_id_token = NULL;
    new_access_token = NULL;
    new_refresh_token = NULL;

    s_login.active = false;
    s_login.completed = true;
    s_login.user_code[0] = '\0';
    s_login.verification_url[0] = '\0';
    s_login.interval = 0;

    strlcpy(s_login.status,
            "connected",
            sizeof(s_login.status));

    strlcpy(s_login.message,
            "ChatGPT connected",
            sizeof(s_login.message));

    state_unlock();

    ESP_LOGI(TAG,
             "ChatGPT OAuth login completed successfully");

    err = ESP_OK;

cleanup:
    free(encoded_code);
    free(encoded_redirect);
    free(encoded_client);
    free(encoded_verifier);
    free(body);

    free(new_id_token);
    free(new_access_token);
    free(new_refresh_token);

    free(transport_error);

    if (root) {
        cJSON_Delete(root);
    }

    claw_llm_http_response_free(&response);

    return err;
}


static esp_err_t refresh_chatgpt_session(const char *stored_refresh_token)
{
    if (!stored_refresh_token || !stored_refresh_token[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *request_json = cJSON_CreateObject();
    if (!request_json) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(request_json, "client_id", OPENAI_CODEX_CLIENT_ID);
    cJSON_AddStringToObject(request_json, "grant_type", "refresh_token");
    cJSON_AddStringToObject(request_json, "refresh_token", stored_refresh_token);

    char *request_body = cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);
    if (!request_body) {
        return ESP_ERR_NO_MEM;
    }

    claw_llm_http_json_request_t request = {0};
    claw_llm_http_response_t response = {0};
    char *transport_error = NULL;

    request.url = OPENAI_CODEX_TOKEN_URL;
    request.body = request_body;
    request.content_type = "application/json";
    request.accept_non_200 = true;
    request.timeout_ms = OPENAI_CODEX_HTTP_TIMEOUT_MS;

    esp_err_t err = claw_llm_http_post_json(&request, &response, &transport_error);
    free(request_body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "ChatGPT refresh transport failed: %s",
                 transport_error ? transport_error : esp_err_to_name(err));
        free(transport_error);
        claw_llm_http_response_free(&response);
        return err;
    }
    free(transport_error);

    if (response.status_code < 200 || response.status_code >= 300) {
        ESP_LOGE(TAG, "ChatGPT refresh returned HTTP %d", response.status_code);
        claw_llm_http_response_free(&response);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response.body ? response.body : "");
    if (!root) {
        claw_llm_http_response_free(&response);
        return ESP_FAIL;
    }

    const char *access_value = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "access_token"));
    const char *id_value = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "id_token"));
    const char *refresh_value = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "refresh_token"));

    if (!access_value || !access_value[0]) {
        cJSON_Delete(root);
        claw_llm_http_response_free(&response);
        ESP_LOGE(TAG, "ChatGPT refresh response has no access_token");
        return ESP_FAIL;
    }

    char *new_access = strdup(access_value);
    char *new_id = (id_value && id_value[0]) ? strdup(id_value) : NULL;
    char *new_refresh = (refresh_value && refresh_value[0])
        ? strdup(refresh_value)
        : strdup(stored_refresh_token);

    if (!new_access || !new_refresh || ((id_value && id_value[0]) && !new_id)) {
        free(new_access);
        free(new_id);
        free(new_refresh);
        cJSON_Delete(root);
        claw_llm_http_response_free(&response);
        return ESP_ERR_NO_MEM;
    }

    if (refresh_value && refresh_value[0] &&
            strcmp(refresh_value, stored_refresh_token) != 0) {
        err = claw_openai_codex_store_save_refresh_token(new_refresh);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "Could not persist rotated refresh token: %s",
                     esp_err_to_name(err));
            free(new_access);
            free(new_id);
            free(new_refresh);
            cJSON_Delete(root);
            claw_llm_http_response_free(&response);
            return err;
        }
    }
    {
        char *account_id = new_id ? extract_chatgpt_account_id(new_id) : NULL;
        if (!account_id) account_id = extract_chatgpt_account_id(new_access);
        if (account_id) {
            esp_err_t account_err = claw_openai_codex_store_save_account_id(account_id);
            if (account_err != ESP_OK) {
                ESP_LOGW(TAG, "Could not update ChatGPT account id: %s", esp_err_to_name(account_err));
            }
            free(account_id);
        }
    }

    state_lock();
    free(s_access_token);
    free(s_id_token);
    free(s_refresh_token);
    s_access_token = new_access;
    s_id_token = new_id;
    s_refresh_token = new_refresh;
    s_login.active = false;
    s_login.completed = true;
    s_login.user_code[0] = '\0';
    s_login.verification_url[0] = '\0';
    strlcpy(s_login.status, "connected", sizeof(s_login.status));
    strlcpy(s_login.message, "ChatGPT connected", sizeof(s_login.message));
    state_unlock();

    cJSON_Delete(root);
    claw_llm_http_response_free(&response);
    ESP_LOGI(TAG, "ChatGPT session restored from refresh token");
    return ESP_OK;
}

static void openai_codex_restore_task(void *arg)
{
    char *refresh_token = (char *)arg;
    esp_err_t err = refresh_chatgpt_session(refresh_token);
    free(refresh_token);

    if (err != ESP_OK) {
        state_set_error("Stored ChatGPT login could not be refreshed");
    }

    worker_finished();
    vTaskDelete(NULL);
}

esp_err_t claw_openai_codex_auth_restore_async(void)
{
    esp_err_t err = ensure_state_lock();
    if (err != ESP_OK) {
        return err;
    }

    state_lock();
    if (s_worker_running || s_login.completed) {
        state_unlock();
        return ESP_OK;
    }
    state_unlock();

    char *refresh_token = NULL;
    err = claw_openai_codex_store_load_refresh_token(&refresh_token);

    if (err == ESP_ERR_NOT_FOUND) {
        state_lock();
        if (!s_login.status[0]) {
            strlcpy(s_login.status, "disconnected", sizeof(s_login.status));
            strlcpy(s_login.message, "ChatGPT not connected", sizeof(s_login.message));
        }
        state_unlock();
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    state_lock();
    if (s_worker_running || s_login.completed) {
        state_unlock();
        free(refresh_token);
        return ESP_OK;
    }

    s_cancel_requested = false;
    s_worker_running = true;
    s_login.active = true;
    s_login.completed = false;
    strlcpy(s_login.status, "refreshing", sizeof(s_login.status));
    strlcpy(s_login.message, "Restoring ChatGPT login", sizeof(s_login.message));
    state_unlock();

    BaseType_t task_ok = xTaskCreate(
        openai_codex_restore_task,
        "codex_restore",
        OPENAI_CODEX_POLL_TASK_STACK,
        refresh_token,
        OPENAI_CODEX_POLL_TASK_PRIORITY,
        NULL);

    if (task_ok != pdPASS) {
        state_lock();
        s_worker_running = false;
        state_unlock();
        free(refresh_token);
        state_set_error("Could not start ChatGPT restore task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void openai_codex_poll_task(void *arg)
{
    openai_codex_poll_args_t *poll =
        (openai_codex_poll_args_t *)arg;

    TickType_t start_tick =
        xTaskGetTickCount();

    if (!poll) {
        state_set_error(
            "OpenAI polling task has no context");

        worker_finished();
        vTaskDelete(NULL);
        return;
    }

    while (!s_cancel_requested) {
        cJSON *request_json = NULL;
        char *request_body = NULL;

        claw_llm_http_json_request_t request = {0};
        claw_llm_http_response_t response = {0};

        char *transport_error = NULL;

        request_json = cJSON_CreateObject();

        if (!request_json) {
            state_set_error(
                "Out of memory creating OpenAI poll request");
            break;
        }

        cJSON_AddStringToObject(
            request_json,
            "device_auth_id",
            poll->device_auth_id);

        cJSON_AddStringToObject(
            request_json,
            "user_code",
            poll->user_code);

        request_body =
            cJSON_PrintUnformatted(request_json);

        cJSON_Delete(request_json);
        request_json = NULL;

        if (!request_body) {
            state_set_error(
                "Out of memory serializing OpenAI poll request");
            break;
        }

        request.url =
            OPENAI_CODEX_DEVICE_TOKEN_URL;

        request.body =
            request_body;

        request.content_type =
            "application/json";

        request.accept_non_200 =
            true;

        request.timeout_ms =
            OPENAI_CODEX_HTTP_TIMEOUT_MS;

        esp_err_t err =
            claw_llm_http_post_json(
                &request,
                &response,
                &transport_error);

        free(request_body);
        request_body = NULL;

        if (s_cancel_requested) {
            free(transport_error);
            claw_llm_http_response_free(&response);
            break;
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "Device-auth poll transport failed: %s",
                     transport_error
                        ? transport_error
                        : esp_err_to_name(err));

            free(transport_error);
            claw_llm_http_response_free(&response);

            state_set_error(
                "OpenAI device-auth polling failed");
            break;
        }

        free(transport_error);
        transport_error = NULL;

        if (response.status_code >= 200 &&
                response.status_code < 300) {

            cJSON *root =
                cJSON_Parse(response.body
                                ? response.body
                                : "");

            if (!root) {
                claw_llm_http_response_free(&response);

                state_set_error(
                    "Could not parse OpenAI authorization response");
                break;
            }

            const char *authorization_code =
                cJSON_GetStringValue(
                    cJSON_GetObjectItemCaseSensitive(
                        root,
                        "authorization_code"));

            const char *code_challenge =
                cJSON_GetStringValue(
                    cJSON_GetObjectItemCaseSensitive(
                        root,
                        "code_challenge"));

            const char *code_verifier =
                cJSON_GetStringValue(
                    cJSON_GetObjectItemCaseSensitive(
                        root,
                        "code_verifier"));

            if (!authorization_code ||
                    !authorization_code[0] ||
                    !code_challenge ||
                    !code_challenge[0] ||
                    !code_verifier ||
                    !code_verifier[0]) {

                cJSON_Delete(root);
                claw_llm_http_response_free(&response);

                state_set_error(
                    "OpenAI authorization response is incomplete");
                break;
            }

            char *authorization_code_copy =
                strdup(authorization_code);

            char *code_verifier_copy =
                strdup(code_verifier);

            cJSON_Delete(root);
            claw_llm_http_response_free(&response);

            if (!authorization_code_copy ||
                    !code_verifier_copy) {

                free(authorization_code_copy);
                free(code_verifier_copy);

                state_set_error(
                    "Out of memory processing OpenAI authorization");
                break;
            }

            if (s_cancel_requested) {
                free(authorization_code_copy);
                free(code_verifier_copy);
                break;
            }

            state_lock();

            strlcpy(s_login.status,
                    "exchanging",
                    sizeof(s_login.status));

            strlcpy(s_login.message,
                    "Finishing ChatGPT login",
                    sizeof(s_login.message));

            state_unlock();

            err = exchange_authorization_code(
                authorization_code_copy,
                code_verifier_copy);

            free(authorization_code_copy);
            free(code_verifier_copy);

            if (err != ESP_OK) {
                state_set_error(
                    "ChatGPT token exchange failed");
            }

            break;
        }

        if (response.status_code != 403 &&
                response.status_code != 404) {

            char message[96];

            snprintf(
                message,
                sizeof(message),
                "OpenAI device auth failed with HTTP %d",
                response.status_code);

            claw_llm_http_response_free(&response);

            state_set_error(message);
            break;
        }

        claw_llm_http_response_free(&response);

        TickType_t elapsed_ticks =
            xTaskGetTickCount() - start_tick;

        uint64_t elapsed_ms =
            (uint64_t)elapsed_ticks *
            (uint64_t)portTICK_PERIOD_MS;

        if (elapsed_ms >=
                ((uint64_t)OPENAI_CODEX_MAX_LOGIN_SECONDS *
                 1000ULL)) {

            state_set_error(
                "ChatGPT login timed out after 15 minutes");
            break;
        }

        uint32_t interval =
            poll->interval
                ? poll->interval
                : OPENAI_CODEX_DEFAULT_INTERVAL;

        vTaskDelay(
            pdMS_TO_TICKS(interval * 1000U));
    }

    free(poll);

    worker_finished();

    vTaskDelete(NULL);
}


esp_err_t claw_openai_codex_auth_get_session(char **out_access_token,
                                              char **out_account_id)
{
    if (!out_access_token || !out_account_id) return ESP_ERR_INVALID_ARG;
    *out_access_token = NULL;
    *out_account_id = NULL;

    esp_err_t err = ensure_state_lock();
    if (err != ESP_OK) return err;

    state_lock();
    if (!s_login.completed || strcmp(s_login.status, "connected") != 0 ||
            !s_access_token || !s_access_token[0]) {
        state_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    char *access_token = strdup(s_access_token);
    state_unlock();
    if (!access_token) return ESP_ERR_NO_MEM;

    char *account_id = NULL;
    err = claw_openai_codex_store_load_account_id(&account_id);
    if (err == ESP_ERR_NOT_FOUND || !account_id || !account_id[0]) {
        free(account_id);
        account_id = extract_chatgpt_account_id(access_token);
        if (!account_id) { free(access_token); return ESP_ERR_NOT_FOUND; }
        esp_err_t save_err = claw_openai_codex_store_save_account_id(account_id);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not persist recovered ChatGPT account id: %s", esp_err_to_name(save_err));
        }
        err = ESP_OK;
    }
    if (err != ESP_OK) { free(access_token); free(account_id); return err; }

    *out_access_token = access_token;
    *out_account_id = account_id;
    return ESP_OK;
}
esp_err_t claw_openai_codex_login_start(void)
{
    esp_err_t err =
        ensure_state_lock();

    if (err != ESP_OK) {
        return err;
    }

    state_lock();

    if (s_worker_running) {
        state_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_cancel_requested = false;

    memset(&s_login, 0, sizeof(s_login));

    s_login.active = true;

    strlcpy(s_login.status,
            "requesting",
            sizeof(s_login.status));

    strlcpy(s_login.message,
            "Requesting ChatGPT device code",
            sizeof(s_login.message));

    state_unlock();

    claw_llm_http_json_request_t request = {0};
    claw_llm_http_response_t response = {0};

    char *transport_error = NULL;

    request.url =
        OPENAI_CODEX_DEVICE_CODE_URL;

    request.body =
        "{\"client_id\":\""
        OPENAI_CODEX_CLIENT_ID
        "\"}";

    request.content_type =
        "application/json";

    request.timeout_ms =
        OPENAI_CODEX_HTTP_TIMEOUT_MS;

    err =
        claw_llm_http_post_json(
            &request,
            &response,
            &transport_error);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Device-code request failed: %s",
                 transport_error
                    ? transport_error
                    : esp_err_to_name(err));

        free(transport_error);

        state_set_error(
            "Could not contact OpenAI authentication server");

        claw_llm_http_response_free(&response);

        return err;
    }

    free(transport_error);

    cJSON *root =
        cJSON_Parse(response.body
                        ? response.body
                        : "");

    if (!root) {
        claw_llm_http_response_free(&response);

        state_set_error(
            "Could not parse OpenAI device-code response");

        return ESP_FAIL;
    }

    const char *device_auth_id =
        cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(
                root,
                "device_auth_id"));

    cJSON *user_code_item =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "user_code");

    if (!cJSON_IsString(user_code_item)) {
        user_code_item =
            cJSON_GetObjectItemCaseSensitive(
                root,
                "usercode");
    }

    const char *user_code =
        cJSON_GetStringValue(user_code_item);

    uint32_t interval =
        parse_interval(
            cJSON_GetObjectItemCaseSensitive(
                root,
                "interval"));

    if (!device_auth_id ||
            !device_auth_id[0] ||
            !user_code ||
            !user_code[0]) {

        cJSON_Delete(root);
        claw_llm_http_response_free(&response);

        state_set_error(
            "OpenAI device-code response is incomplete");

        return ESP_FAIL;
    }

    openai_codex_poll_args_t *poll =
        calloc(1, sizeof(*poll));

    if (!poll) {
        cJSON_Delete(root);
        claw_llm_http_response_free(&response);

        state_set_error(
            "Out of memory starting ChatGPT login");

        return ESP_ERR_NO_MEM;
    }

    if (strlen(device_auth_id) >=
            sizeof(poll->device_auth_id) ||
        strlen(user_code) >=
            sizeof(poll->user_code)) {

        free(poll);
        cJSON_Delete(root);
        claw_llm_http_response_free(&response);

        state_set_error(
            "OpenAI device-code response is too large");

        return ESP_ERR_INVALID_SIZE;
    }

    strlcpy(
        poll->device_auth_id,
        device_auth_id,
        sizeof(poll->device_auth_id));

    strlcpy(
        poll->user_code,
        user_code,
        sizeof(poll->user_code));

    poll->interval = interval;

    state_lock();

    s_login.active = true;
    s_login.completed = false;
    s_login.interval = interval;

    strlcpy(
        s_login.user_code,
        user_code,
        sizeof(s_login.user_code));

    strlcpy(
        s_login.verification_url,
        OPENAI_CODEX_VERIFICATION_URL,
        sizeof(s_login.verification_url));

    strlcpy(
        s_login.status,
        "waiting",
        sizeof(s_login.status));

    strlcpy(
        s_login.message,
        "Waiting for ChatGPT authorization",
        sizeof(s_login.message));

    s_worker_running = true;

    state_unlock();

    cJSON_Delete(root);
    claw_llm_http_response_free(&response);

    BaseType_t task_ok =
        xTaskCreate(
            openai_codex_poll_task,
            "codex_oauth",
            OPENAI_CODEX_POLL_TASK_STACK,
            poll,
            OPENAI_CODEX_POLL_TASK_PRIORITY,
            NULL);

    if (task_ok != pdPASS) {
        state_lock();
        s_worker_running = false;
        state_unlock();

        free(poll);

        state_set_error(
            "Could not start ChatGPT login task");

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "OpenAI device login started; poll interval=%u seconds",
        (unsigned)interval);

    return ESP_OK;
}


esp_err_t claw_openai_codex_login_get_status(
    claw_openai_codex_login_status_t *status)
{
    esp_err_t err =
        ensure_state_lock();

    if (err != ESP_OK) {
        return err;
    }

    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    state_lock();

    memset(status, 0, sizeof(*status));

    status->active =
        s_login.active;

    status->completed =
        s_login.completed;

    status->interval =
        s_login.interval;

    strlcpy(
        status->status,
        s_login.status,
        sizeof(status->status));

    strlcpy(
        status->message,
        s_login.message,
        sizeof(status->message));

    strlcpy(
        status->user_code,
        s_login.user_code,
        sizeof(status->user_code));

    strlcpy(
        status->verification_url,
        s_login.verification_url,
        sizeof(status->verification_url));

    state_unlock();

    return ESP_OK;
}


esp_err_t claw_openai_codex_login_cancel(void)
{
    esp_err_t err =
        ensure_state_lock();

    if (err != ESP_OK) {
        return err;
    }

    state_lock();

    /*
     * Do not turn an already completed login into "cancelled".
     * This endpoint only cancels an active login attempt.
     */
    if (!s_worker_running) {
        state_unlock();
        return ESP_OK;
    }

    s_cancel_requested = true;

    s_login.active = false;
    s_login.completed = false;

    strlcpy(
        s_login.status,
        "cancelled",
        sizeof(s_login.status));

    strlcpy(
        s_login.message,
        "ChatGPT login cancelled",
        sizeof(s_login.message));

    state_unlock();

    return ESP_OK;
}