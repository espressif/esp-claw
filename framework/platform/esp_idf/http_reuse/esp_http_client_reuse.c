/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/queue.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_HTTP_REUSE_ENABLE
#include "http_parser.h"

/*
 * Real IDF symbols, exposed by GNU ld --wrap=esp_http_client_init /
 * --wrap=esp_http_client_cleanup / --wrap=esp_http_client_perform set up
 * in this component's CMakeLists.txt. Calling them here bypasses the
 * wrappers below to avoid recursion.
 */
esp_http_client_handle_t __real_esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t                __real_esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t                __real_esp_http_client_perform(esp_http_client_handle_t client);

static const char *TAG = "http_reuse";

typedef struct http_reuse_endpoint {
    esp_http_client_transport_t transport;
    char                       *host;
    int                         port;
} http_reuse_endpoint_t;

typedef struct http_reuse_node {
    STAILQ_ENTRY(http_reuse_node) list;
    esp_http_client_handle_t client;
    http_reuse_endpoint_t    endpoint;
    TickType_t               last_update_ticks;
    bool                     is_persistent;
    /** True while the app holds @p client; idle reusable entries have leased false. */
    bool                     leased;
    /** True if the current lease was satisfied by a pool hit; controls perform retry. */
    bool                     reused_in_current_lease;
} http_reuse_node_t;

static STAILQ_HEAD(http_reuse_head, http_reuse_node)
    s_pool = STAILQ_HEAD_INITIALIZER(s_pool);

static SemaphoreHandle_t s_pool_mutex            = NULL;
static portMUX_TYPE      s_pool_mutex_init_mux   = portMUX_INITIALIZER_UNLOCKED;

/** Frees @p ep->host and clears the pointer. Safe on cleared / transferred endpoints. */
static void endpoint_release(http_reuse_endpoint_t *ep)
{
    if (!ep) {
        return;
    }
    if (ep->host) {
        free(ep->host);
        ep->host = NULL;
    }
    ep->port = 0;
}

static void pool_mutex_take(void)
{
    portENTER_CRITICAL(&s_pool_mutex_init_mux);
    if (s_pool_mutex == NULL) {
        s_pool_mutex = xSemaphoreCreateMutex();
    }
    portEXIT_CRITICAL(&s_pool_mutex_init_mux);
    if (s_pool_mutex) {
        xSemaphoreTake(s_pool_mutex, portMAX_DELAY);
    }
}

static void pool_mutex_give(void)
{
    if (s_pool_mutex) {
        xSemaphoreGive(s_pool_mutex);
    }
}

/** Compare URL fragment [off, off+len) to literal (ASCII), case-insensitive. */
static bool url_field_str_eq_ci(const char *buf, uint16_t off, uint16_t len, const char *lit)
{
    size_t llen = strlen(lit);
    return (len == llen) && (strncasecmp(buf + off, lit, len) == 0);
}

/**
 * Parse scheme/host/effective port from a full URL into @p ep.
 * Replaces any previous @p ep->host (freed first).
 */
static esp_err_t endpoint_from_url(const char *url, http_reuse_endpoint_t *ep)
{
    if (!url || !ep) {
        return ESP_ERR_INVALID_ARG;
    }

    endpoint_release(ep);

    struct http_parser_url u;
    http_parser_url_init(&u);
    if (http_parser_parse_url(url, strlen(url), 0, &u) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!(u.field_set & (1 << UF_SCHEMA)) || !(u.field_set & (1 << UF_HOST))) {
        return ESP_ERR_INVALID_ARG;
    }

    int default_port = 0;
    if (url_field_str_eq_ci(url, u.field_data[UF_SCHEMA].off, u.field_data[UF_SCHEMA].len, "https")) {
        ep->transport = HTTP_TRANSPORT_OVER_SSL;
        default_port  = 443;
    } else if (url_field_str_eq_ci(url, u.field_data[UF_SCHEMA].off, u.field_data[UF_SCHEMA].len, "http")) {
        ep->transport = HTTP_TRANSPORT_OVER_TCP;
        default_port  = 80;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    size_t host_len = u.field_data[UF_HOST].len;
    if (host_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ep->host = calloc(1, host_len + 1);
    if (!ep->host) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(ep->host, url + u.field_data[UF_HOST].off, host_len);
    ep->host[host_len] = '\0';

    if (u.field_set & (1 << UF_PORT)) {
        ep->port = (int)u.port;
    } else {
        ep->port = default_port;
    }

    return ESP_OK;
}

/** Max buffer for esp_http_client_get_url when deriving endpoint at cleanup (not in pool yet). */
#define HTTP_REUSE_CLIENT_URL_BUF 256

static esp_err_t endpoint_from_client(esp_http_client_handle_t client, http_reuse_endpoint_t *ep)
{
    if (!client || !ep) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[HTTP_REUSE_CLIENT_URL_BUF] = {0};
    esp_err_t r = esp_http_client_get_url(client, url, sizeof(url));
    if (r != ESP_OK) {
        return r;
    }
    return endpoint_from_url(url, ep);
}

static esp_err_t endpoint_from_config(const esp_http_client_config_t *config, http_reuse_endpoint_t *ep)
{
    if (!config || !ep) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Use URL if provided */
    if (config->url && config->url[0]) {
        return endpoint_from_url(config->url, ep);
    }

    /* Use host if provided */
    if (!config->host || !config->host[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    endpoint_release(ep);
    ep->host = strdup(config->host);
    if (!ep->host) {
        return ESP_ERR_NO_MEM;
    }
    ep->transport = config->transport_type;
    ep->port      = config->port > 0 ? config->port
                                     : (config->transport_type == HTTP_TRANSPORT_OVER_SSL ? 443 : 80);

    return ESP_OK;
}

static bool endpoint_match(const http_reuse_endpoint_t *a, const http_reuse_endpoint_t *b)
{
    if (!a->host || !b->host) {
        return false;
    }
    return (a->transport == b->transport) && (a->port == b->port)
           && (strcasecmp(a->host, b->host) == 0);
}

static size_t pool_count_locked(void)
{
    size_t             n = 0;
    http_reuse_node_t *node;

    STAILQ_FOREACH(node, &s_pool, list) {
        n++;
    }
    return n;
}

static void node_free(http_reuse_node_t *node, bool destroy_client)
{
    if (!node) {
        return;
    }
    if (destroy_client && node->client) {
        __real_esp_http_client_cleanup(node->client);
    }
    endpoint_release(&node->endpoint);
    free(node);
}

static esp_http_client_handle_t pool_take_locked(const http_reuse_endpoint_t *target)
{
    http_reuse_node_t *node;
    http_reuse_node_t *next;

    for (node = STAILQ_FIRST(&s_pool); node != NULL; node = next) {
        next = STAILQ_NEXT(node, list);

        if (!node->is_persistent) {
            /* Init registered this handle; not idle for reuse yet. */
            continue;
        }
        if (node->leased) {
            continue;
        }
        if (!endpoint_match(target, &node->endpoint)) {
            continue;
        }

        int client_errno = esp_http_client_get_errno(node->client);
        if (client_errno != 0) {
            ESP_LOGE(TAG, "Pooled client %p has errno=%d, dropping from reuse pool", node->client,
                     client_errno);
            STAILQ_REMOVE(&s_pool, node, http_reuse_node, list);
            node_free(node, true);
            return NULL;
        }
        node->leased                  = true;
        node->reused_in_current_lease = true;
        node->last_update_ticks       = xTaskGetTickCount();
        return node->client;
    }
    return NULL;
}

static http_reuse_node_t *pool_find_locked(esp_http_client_handle_t client)
{
    http_reuse_node_t *node;
    STAILQ_FOREACH(node, &s_pool, list) {
        if (node->client == client) {
            return node;
        }
    }
    return NULL;
}

/** Caller must hold @ref pool_mutex. Removes @p client from the pool and destroys it, or only destroys if not listed. */
static void pool_detach_and_destroy_client_locked(esp_http_client_handle_t client)
{
    http_reuse_node_t *n = pool_find_locked(client);
    if (n) {
        STAILQ_REMOVE(&s_pool, n, http_reuse_node, list);
        node_free(n, true);
    } else {
        (void)__real_esp_http_client_cleanup(client);
    }
}

/**
 * Insert a client into the pool.
 * @p is_persistent / @p leased describe the slot: idle reusable connections use
 * (true, false); a handle taken from the pool for a new lease uses (true, true)
 * via pool_take_locked, not this helper.
 *
 * When full, only LRU-evicts entries with is_persistent true and leased false
 * (idle reuse slots).
 */
static esp_err_t pool_insert_locked(esp_http_client_handle_t client, http_reuse_endpoint_t *ep,
                                    bool is_persistent, bool leased)
{
    if (!client || !ep) {
        return ESP_ERR_INVALID_ARG;
    }

    while (pool_count_locked() >= (size_t)CONFIG_HTTP_REUSE_MAX_POOL) {
        http_reuse_node_t *node;
        http_reuse_node_t *oldest       = NULL;
        TickType_t         oldest_ticks = portMAX_DELAY;

        STAILQ_FOREACH(node, &s_pool, list) {
            if (!node->is_persistent || node->leased) {
                continue;
            }
            if (node->last_update_ticks < oldest_ticks) {
                oldest_ticks = node->last_update_ticks;
                oldest       = node;
            }
        }
        if (!oldest) {
            return ESP_ERR_NO_MEM;
        }
        STAILQ_REMOVE(&s_pool, oldest, http_reuse_node, list);
        ESP_LOGI(TAG, "evict LRU idle persistent %p (pool full)", oldest->client);
        node_free(oldest, true);
    }

    http_reuse_node_t *new_node = calloc(1, sizeof(*new_node));
    if (!new_node) {
        return ESP_ERR_NO_MEM;
    }
    new_node->client                  = client;
    new_node->endpoint.transport      = ep->transport;
    new_node->endpoint.port           = ep->port;
    new_node->endpoint.host           = ep->host;
    ep->host                          = NULL; /* ownership moved into pool node */
    new_node->last_update_ticks       = xTaskGetTickCount();
    new_node->is_persistent           = is_persistent;
    new_node->leased                  = leased;
    new_node->reused_in_current_lease = false; /* freshly inserted, not a pool hit */
    STAILQ_INSERT_TAIL(&s_pool, new_node, list);
    return ESP_OK;
}

/*
 * --wrap entry points. The IDF callers (and any application code) that invoke
 * esp_http_client_init / _cleanup / _perform are linked here when
 * CONFIG_HTTP_REUSE_ENABLE=y. These wrappers manage the connection pool
 * around the real IDF implementation reachable via __real_*.
 */

esp_http_client_handle_t __wrap_esp_http_client_init(const esp_http_client_config_t *config)
{
    if (!config) {
        return NULL;
    }

    /* Local shallow copy so we can force keep-alive without mutating caller's config. */
    esp_http_client_config_t cfg = *config;
    if (!cfg.keep_alive_enable) {
        cfg.keep_alive_enable   = true;
        cfg.keep_alive_idle     = 5;
        cfg.keep_alive_interval = 5;
        cfg.keep_alive_count    = 3;
    }

    ESP_LOGD(TAG, "init url=%s", cfg.url ? cfg.url : (cfg.host ? cfg.host : "(none)"));

    http_reuse_endpoint_t target = {0};
    esp_err_t             err    = endpoint_from_config(&cfg, &target);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "endpoint_from_config failed (%s), bypass pool (direct init)",
                 esp_err_to_name(err));
        return __real_esp_http_client_init(&cfg);
    }

    pool_mutex_take();
    esp_http_client_handle_t client = pool_take_locked(&target);
    pool_mutex_give();

    if (client) {
        endpoint_release(&target);
        bool reuse_ok = true;
        /* If the host, transport and port are the same and path is different, update the path depending on the new url */
        if (cfg.url && cfg.url[0]) {
            esp_err_t set_err = esp_http_client_set_url(client, cfg.url);
            if (set_err != ESP_OK) {
                ESP_LOGE(TAG, "set_url failed (%s), discarding pooled client %p", esp_err_to_name(set_err),
                         client);
                reuse_ok = false;
            }
        } else if (cfg.host && cfg.host[0] && cfg.path && cfg.path[0]) { /* If the host, transport, port and path is provided, update the url */
            /* Convert the host, transport, port and path to a url */
            const bool  tls    = (cfg.transport_type == HTTP_TRANSPORT_OVER_SSL);
            const char *scheme = tls ? "https" : "http";
            int         port   = cfg.port > 0 ? cfg.port : (tls ? 443 : 80);
            size_t      cap    = strlen(scheme) + 3 + strlen(cfg.host) + 12 + strlen(cfg.path) + 1;
            char       *url    = calloc(1, cap);
            if (!url) {
                ESP_LOGE(TAG, "calloc url failed (host/path reuse), discarding pooled client %p", client);
                pool_mutex_take();
                pool_detach_and_destroy_client_locked(client);
                pool_mutex_give();
                return NULL;
            }
            snprintf(url, cap, "%s://%s:%d%s", scheme, cfg.host, port, cfg.path);
            ESP_LOGD(TAG, "reuse set_url %s", url);
            esp_err_t set_err = esp_http_client_set_url(client, url);
            free(url);
            if (set_err != ESP_OK) {
                ESP_LOGE(TAG, "set_url failed (%s), discarding pooled client %p", esp_err_to_name(set_err),
                         client);
                reuse_ok = false;
            }
        }
        if (!reuse_ok) {
            pool_mutex_take();
            pool_detach_and_destroy_client_locked(client);
            pool_mutex_give();
            return NULL;
        }
        /*
         * Pooled handles keep internal request state from the previous lease.
         * Mirror fresh-init semantics so this lease matches @p cfg before the
         * app calls set_post_field / perform (method, stale POST body, timeout, user_data).
         * Default method is GET.
         */
        esp_http_client_set_method(client, cfg.method);
        esp_http_client_set_post_field(client, NULL, 0);
        const int timeout_ms = cfg.timeout_ms > 0 ? cfg.timeout_ms : 5000;
        esp_http_client_set_timeout_ms(client, timeout_ms);
        esp_http_client_set_user_data(client, cfg.user_data);
        ESP_LOGI(TAG, "reuse hit %p url=%s", client,
                 cfg.url ? cfg.url : (cfg.host ? cfg.host : "(none)"));
        return client;
    }

    ESP_LOGD(TAG, "new client url=%s", cfg.url ? cfg.url : (cfg.host ? cfg.host : "(none)"));
    esp_http_client_handle_t new_client = __real_esp_http_client_init(&cfg);
    if (!new_client) {
        endpoint_release(&target);
        return NULL;
    }

    /*
     * Do not register in the pool at init. Handles that are never cleaned up
     * would fill CONFIG_HTTP_REUSE_MAX_POOL with leased non-persistent slots
     * that cannot be LRU-evicted, breaking further inits. Idle reuse entries are
     * added only in cleanup when the connection is persistent.
     */
    endpoint_release(&target);
    return new_client;
}

esp_err_t __wrap_esp_http_client_cleanup(esp_http_client_handle_t client)
{
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "cleanup %p", client);

    pool_mutex_take();
    http_reuse_node_t *node       = pool_find_locked(client);
    bool               in_pool    = (node != NULL);
    bool               persistent = esp_http_client_is_persistent_connection(client);

    if (in_pool) {
        if (!persistent) {
            STAILQ_REMOVE(&s_pool, node, http_reuse_node, list);
            pool_mutex_give();
            ESP_LOGD(TAG, "remove non-persistent pooled %p", client);
            node_free(node, true);
            return ESP_OK;
        }
        node->is_persistent           = true;
        node->leased                  = false; /* idle in pool for pool_take */
        node->reused_in_current_lease = false; /* lease ended */
        node->last_update_ticks       = xTaskGetTickCount();
        pool_mutex_give();
        ESP_LOGD(TAG, "idle persistent in pool %p", client);
        return ESP_OK;
    }
    pool_mutex_give();

    /* Not in pool: either never registered (new init path) or foreign handle. */
    if (!persistent) {
        ESP_LOGD(TAG, "cleanup destroy (not in pool) %p", client);
        return __real_esp_http_client_cleanup(client);
    }

    /* Persistent connection after use: retain for pool_take without holding a slot during init. */
    http_reuse_endpoint_t ep = {0};
    esp_err_t             epe = endpoint_from_client(client, &ep);
    if (epe != ESP_OK) {
        ESP_LOGE(TAG, "persistent %p get_url/endpoint failed (%s), destroy", client, esp_err_to_name(epe));
        return __real_esp_http_client_cleanup(client);
    }

    pool_mutex_take();
    esp_err_t pin_err = pool_insert_locked(client, &ep, true, false);
    pool_mutex_give();
    if (pin_err != ESP_OK) {
        ESP_LOGE(TAG, "pool insert persistent %p failed (%s), destroy", client, esp_err_to_name(pin_err));
        endpoint_release(&ep);
        return __real_esp_http_client_cleanup(client);
    }
    ESP_LOGD(TAG, "insert idle persistent %p", client);
    return ESP_OK;
}

esp_err_t __wrap_esp_http_client_perform(esp_http_client_handle_t client)
{
    esp_err_t err = __real_esp_http_client_perform(client);
    if (err == ESP_OK || !client) {
        return err;
    }

    pool_mutex_take();
    http_reuse_node_t *node  = pool_find_locked(client);
    bool               retry = (node != NULL) && node->reused_in_current_lease;
    if (retry) {
        /* Only allow a single retry per lease, even if the app calls perform again. */
        node->reused_in_current_lease = false;
    }
    pool_mutex_give();

    if (retry) {
        ESP_LOGI(TAG, "perform %s on reused %p, close+retry once", esp_err_to_name(err), client);
        (void)esp_http_client_close(client);
        err = __real_esp_http_client_perform(client);
    }
    return err;
}

#endif /* CONFIG_HTTP_REUSE_ENABLE */
