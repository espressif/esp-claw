/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mcp_mdns.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/ip_addr.h"
#include "mdns.h"

static const char *TAG = "mcp_mdns";
static bool mdns_init_done = false;

static const char *mcp_mdns_find_txt_value(const mdns_result_t *result, const char *key)
{
    if (!result || !key) {
        return NULL;
    }

    for (size_t i = 0; i < result->txt_count; i++) {
        if (result->txt[i].key && strcmp(result->txt[i].key, key) == 0) {
            return result->txt[i].value;
        }
    }

    return NULL;
}

static const char *mcp_mdns_pick_ip_string(const mdns_result_t *result,
                                           char *buf,
                                           size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return NULL;
    }
    buf[0] = '\0';

    if (!result || !result->addr) {
        return NULL;
    }

    for (const mdns_ip_addr_t *addr = result->addr; addr; addr = addr->next) {
        if (ipaddr_ntoa_r((const ip_addr_t *)&addr->addr, buf, buf_size)) {
            return buf;
        }
    }

    return NULL;
}

static const char *mcp_mdns_get_self_ip_string(char *buf, size_t buf_size)
{
    esp_netif_t *netif = NULL;
    esp_netif_ip_info_t ip_info = {0};

    if (!buf || buf_size == 0) {
        return NULL;
    }
    buf[0] = '\0';

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return NULL;
    }
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return NULL;
    }

    snprintf(buf, buf_size, IPSTR, IP2STR(&ip_info.ip));
    return buf;
}

static esp_err_t mcp_mdns_append_device(cJSON *devices,
                                        const char *instance,
                                        const char *hostname,
                                        const char *ip,
                                        const char *url_host,
                                        uint16_t port,
                                        const char *endpoint)
{
    cJSON *device = NULL;
    char server_url[320];
    char url[384];
    const char *host_for_url = NULL;

    if (!devices || !hostname || !hostname[0] || !endpoint || !endpoint[0] || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    host_for_url = (url_host && url_host[0]) ? url_host : ((ip && ip[0]) ? ip : hostname);
    snprintf(server_url, sizeof(server_url), "http://%s:%u", host_for_url, (unsigned int)port);
    snprintf(url, sizeof(url), "%s/%s", server_url, endpoint);

    device = cJSON_CreateObject();
    if (!device) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(device, "instance", (instance && instance[0]) ? instance : "(unknown)");
    cJSON_AddStringToObject(device, "hostname", hostname);
    cJSON_AddStringToObject(device, "ip", (ip && ip[0]) ? ip : "(unresolved)");
    cJSON_AddNumberToObject(device, "port", port);
    cJSON_AddStringToObject(device, "endpoint", endpoint);
    cJSON_AddStringToObject(device, "server_url", server_url);
    cJSON_AddStringToObject(device, "url", url);
    cJSON_AddItemToArray(devices, device);
    return ESP_OK;
}

static bool mcp_mdns_devices_has_match(const cJSON *devices,
                                       const char *hostname,
                                       uint16_t port,
                                       const char *endpoint)
{
    const cJSON *device = NULL;

    if (!cJSON_IsArray((cJSON *)devices) || !hostname || !hostname[0] || !endpoint || !endpoint[0]) {
        return false;
    }

    cJSON_ArrayForEach(device, (cJSON *)devices) {
        const cJSON *hostname_item = cJSON_GetObjectItemCaseSensitive((cJSON *)device, "hostname");
        const cJSON *port_item = cJSON_GetObjectItemCaseSensitive((cJSON *)device, "port");
        const cJSON *endpoint_item = cJSON_GetObjectItemCaseSensitive((cJSON *)device, "endpoint");

        if (!cJSON_IsString((cJSON *)hostname_item) || !cJSON_IsNumber((cJSON *)port_item) ||
                !cJSON_IsString((cJSON *)endpoint_item)) {
            continue;
        }
        if (strcmp(hostname_item->valuestring, hostname) == 0 &&
                (uint16_t)port_item->valueint == port &&
                strcmp(endpoint_item->valuestring, endpoint) == 0) {
            return true;
        }
    }

    return false;
}

static esp_err_t mcp_mdns_append_self_device_if_needed(cJSON *devices,
                                                       const mcp_mdns_query_config_t *config,
                                                       size_t *found)
{
    const mcp_mdns_service_info_t *self = config ? config->self : NULL;
    char hostname_local[96];
    char ip_buf[64];
    const char *hostname = NULL;
    const char *instance = NULL;
    const char *endpoint = NULL;
    const char *ip = NULL;
    esp_err_t err;

    if (!devices || !found || !self) {
        return ESP_OK;
    }

    hostname = (self->hostname && self->hostname[0]) ? self->hostname : config->default_hostname;
    instance = (self->instance_name && self->instance_name[0]) ? self->instance_name : "(unknown)";
    endpoint = (self->endpoint && self->endpoint[0]) ? self->endpoint : config->default_endpoint;
    if (!hostname || !hostname[0] || !endpoint || !endpoint[0] || self->port == 0) {
        return ESP_OK;
    }

    ip = mcp_mdns_get_self_ip_string(ip_buf, sizeof(ip_buf));
    if (mcp_mdns_devices_has_match(devices, hostname, self->port, endpoint)) {
        return ESP_OK;
    }

    snprintf(hostname_local, sizeof(hostname_local), "%s.local", hostname);
    err = mcp_mdns_append_device(devices,
                                 instance,
                                 hostname,
                                 ip,
                                 ip ? ip : hostname_local,
                                 self->port,
                                 endpoint);
    if (err == ESP_OK) {
        (*found)++;
    }
    return err;
}

esp_err_t mcp_mdns_init(const char *hostname, const char *instance_name)
{
    if (mdns_init_done) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "Failed to initialize mDNS");

    if (hostname && hostname[0]) {
        ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), TAG, "Failed to set mDNS hostname");
    }

    if (instance_name && instance_name[0]) {
        ESP_RETURN_ON_ERROR(mdns_instance_name_set(instance_name), TAG, "Failed to set mDNS instance name");
    }

    mdns_init_done = true;
    return ESP_OK;
}

esp_err_t mcp_mdns_deinit(void)
{
    mdns_free();
    mdns_init_done = false;

    return ESP_OK;
}

esp_err_t mcp_mdns_add_service(const mcp_mdns_service_info_t *service)
{
    mdns_txt_item_t txt[] = {
        {"endpoint", service ? service->endpoint : NULL},
    };

    if (!service || !service->instance_name || !service->instance_name[0] ||
        !service->endpoint || !service->endpoint[0] || service->port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(mcp_mdns_init(service->hostname, service->instance_name), TAG, "Failed to initialize mDNS");
    ESP_RETURN_ON_ERROR(mdns_service_add(service->instance_name, MCP_MDNS_SERVICE_TYPE, MCP_MDNS_SERVICE_PROTO, service->port, txt, sizeof(txt) / sizeof(txt[0])), TAG, "Failed to add mDNS service");
    ESP_RETURN_ON_ERROR(mdns_service_port_set(MCP_MDNS_SERVICE_TYPE, MCP_MDNS_SERVICE_PROTO, service->port), TAG, "Failed to set mDNS service port");
    ESP_RETURN_ON_ERROR(mdns_service_txt_set(MCP_MDNS_SERVICE_TYPE, MCP_MDNS_SERVICE_PROTO, txt, sizeof(txt) / sizeof(txt[0])), TAG, "Failed to set mDNS service txt");

    return ESP_OK;
}

esp_err_t mcp_mdns_remove_service(void)
{
    return mdns_service_remove(MCP_MDNS_SERVICE_TYPE, MCP_MDNS_SERVICE_PROTO);
}

esp_err_t mcp_mdns_query_devices(const mcp_mdns_query_config_t *config, cJSON **result_out)
{
    int timeout_ms = MCP_MDNS_DEFAULT_TIMEOUT_MS;
    bool include_self = true;
    const char *default_hostname = NULL;
    const char *default_endpoint = NULL;
    mdns_result_t *results = NULL;
    cJSON *root = NULL;
    cJSON *devices = NULL;
    size_t found = 0;
    esp_err_t err;

    if (!result_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *result_out = NULL;

    if (config) {
        timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : MCP_MDNS_DEFAULT_TIMEOUT_MS;
        include_self = config->include_self;
        default_hostname = config->default_hostname;
        default_endpoint = config->default_endpoint;
    }

    ESP_RETURN_ON_ERROR(mcp_mdns_init(NULL, NULL), TAG, "Failed to initialize mDNS");
    ESP_RETURN_ON_ERROR(mdns_query_ptr(MCP_MDNS_SERVICE_TYPE, MCP_MDNS_SERVICE_PROTO, timeout_ms, 20, &results), TAG, "Failed to query mDNS");

    root = cJSON_CreateObject();
    devices = cJSON_CreateArray();
    if (!root || !devices) {
        mdns_query_results_free(results);
        cJSON_Delete(root);
        cJSON_Delete(devices);
        return ESP_ERR_NO_MEM;
    }

    for (mdns_result_t *result = results; result; result = result->next) {
        char ip_buf[64];
        const char *ip = NULL;
        const char *endpoint = NULL;
        const char *hostname = NULL;
        const char *instance = NULL;
        esp_err_t append_err;

        if (!include_self && default_hostname && result->hostname &&
            strcmp(result->hostname, default_hostname) == 0) {
            continue;
        }

        ip = mcp_mdns_pick_ip_string(result, ip_buf, sizeof(ip_buf));
        endpoint = mcp_mdns_find_txt_value(result, "endpoint");
        if (!endpoint || !endpoint[0]) {
            endpoint = default_endpoint;
        }

        hostname = (result->hostname && result->hostname[0]) ? result->hostname : "(unknown)";
        instance = (result->instance_name && result->instance_name[0]) ? result->instance_name : "(unknown)";

        append_err = mcp_mdns_append_device(devices, instance, hostname, ip, NULL, result->port, endpoint);
        if (append_err != ESP_OK) {
            mdns_query_results_free(results);
            cJSON_Delete(root);
            return append_err;
        }
        found ++;
    }

    mdns_query_results_free(results);
    if (include_self) {
        err = mcp_mdns_append_self_device_if_needed(devices, config, &found);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            return err;
        }
    }

    cJSON_AddNumberToObject(root, "count", (double)found);
    cJSON_AddItemToObject(root, "devices", devices);
    *result_out = root;
    return ESP_OK;
}
