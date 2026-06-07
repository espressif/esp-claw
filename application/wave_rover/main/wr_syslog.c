/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP syslog forwarder — mirrors every ESP-IDF log line to broadcast:514
 * so the rover can be monitored wirelessly without a serial cable.
 */
#include "wr_syslog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define SYSLOG_PORT_DEFAULT   5514
#define SYSLOG_FACILITY_DEFAULT 16  /* local0 */
#define MSG_MAX       480
#define QUEUE_DEPTH   24

static QueueHandle_t s_queue    = NULL;
static int           s_sock     = -1;
static uint8_t       s_facility = SYSLOG_FACILITY_DEFAULT;

/* ------------------------------------------------------------------ */
/* vprintf hook — runs in the caller's task under the ESP log lock     */
/* ------------------------------------------------------------------ */

static int syslog_vprintf(const char *fmt, va_list args)
{
    /* UART output first (must consume args) */
    va_list copy;
    va_copy(copy, args);
    int ret = vprintf(fmt, args);

    /* Enqueue for UDP — non-blocking, drop on overflow */
    if (s_queue) {
        char buf[MSG_MAX];
        vsnprintf(buf, sizeof(buf), fmt, copy);
        /* strip trailing newline so syslog viewer adds its own */
        int len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
        if (len > 0)
            xQueueSend(s_queue, buf, 0);
    }
    va_end(copy);
    return ret;
}

/* ------------------------------------------------------------------ */
/* JSON formatting for the UDP-forwarded copy (UART stays plain text)  */
/* ------------------------------------------------------------------ */

/* Copies up to out_sz-1 bytes from in[0..in_len) into out, escaping the
 * characters that are illegal or special inside a JSON string (quote,
 * backslash, control characters). Always NUL-terminates. Truncates
 * silently on overflow rather than overrunning out — acceptable for a
 * best-effort log line; never crashes or writes out of bounds. */
static void json_escape(const char *in, size_t in_len, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len && in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        char seq[8];
        const char *esc = NULL;
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (c < 0x20) {
                snprintf(seq, sizeof(seq), "\\u%04x", c);
                esc = seq;
            }
            break;
        }
        size_t add_len = esc ? strlen(esc) : 1;
        if (o + add_len > out_sz - 1) break;
        if (esc) {
            memcpy(out + o, esc, add_len);
        } else {
            out[o] = (char)c;
        }
        o += add_len;
    }
    out[o] = '\0';
}

/* Parses a rendered ESP-IDF log line "L (TTTT) TAG: message" (the fixed
 * LOG_FORMAT shape with CONFIG_LOG_COLORS unset — confirmed in
 * sdkconfig.wave_rover) and writes a single-line JSON object to out.
 * Lines that don't match the expected shape (e.g. raw printf output, the
 * "wr_syslog: forwarding logs to ..." startup line) fall back to emitting
 * the whole line as "msg" with level "info" and tag "" — never dropped,
 * never misparsed into a crash. */
static void format_json(const char *line, char *out, size_t out_sz)
{
    const char *level_word = "info";
    unsigned long ts = 0;
    const char *tag = "";
    size_t tag_len = 0;
    const char *msg = line;

    if (line[0] != '\0' && line[1] == ' ' && line[2] == '(') {
        switch (line[0]) {
        case 'E': level_word = "error";   break;
        case 'W': level_word = "warn";    break;
        case 'I': level_word = "info";    break;
        case 'D': level_word = "debug";   break;
        case 'V': level_word = "verbose"; break;
        default:                          break;
        }
        char *end = NULL;
        ts = strtoul(line + 3, &end, 10);
        if (end != NULL && end[0] == ')' && end[1] == ' ') {
            const char *tag_start = end + 2;
            const char *colon = strstr(tag_start, ": ");
            if (colon != NULL) {
                tag     = tag_start;
                tag_len = (size_t)(colon - tag_start);
                msg     = colon + 2;
            }
        }
    }

    char tag_buf[32];
    char msg_buf[MSG_MAX];
    json_escape(tag, tag_len, tag_buf, sizeof(tag_buf));
    json_escape(msg, strlen(msg), msg_buf, sizeof(msg_buf));

    snprintf(out, out_sz,
             "{\"ts\":%lu,\"level\":\"%s\",\"tag\":\"%s\",\"msg\":\"%s\"}",
             ts, level_word, tag_buf, msg_buf);
}

/* ------------------------------------------------------------------ */
/* UDP sender task                                                     */
/* ------------------------------------------------------------------ */

static void syslog_task(void *arg)
{
    char msg[MSG_MAX];
    char json[MSG_MAX + 96];
    char pkt[sizeof(json) + 32];

    for (;;) {
        if (xQueueReceive(s_queue, msg, portMAX_DELAY) != pdTRUE) continue;
        int fd = s_sock;
        if (fd < 0) continue;

        format_json(msg, json, sizeof(json));

        /* RFC 3164: <PRI>HOSTNAME TAG: MSG
         * PRI = facility*8 | severity (6 = info) — MSG is now a JSON object
         * so a Loki "syslog" receiver + "json" pipeline stage can extract
         * level/tag/msg as labels without per-line regex parsing. */
        int pri = (int)s_facility * 8 + 6;
        int n = snprintf(pkt, sizeof(pkt), "<%d>wave-rover: %s", pri, json);
        if (n > 0)
            send(fd, pkt, (size_t)n, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t wr_syslog_init(void)
{
    s_queue = xQueueCreate(QUEUE_DEPTH, MSG_MAX);
    if (!s_queue) return ESP_ERR_NO_MEM;

    esp_log_set_vprintf(syslog_vprintf);

    xTaskCreate(syslog_task, "wr_syslog", 4096, NULL, 2, NULL);
    return ESP_OK;
}

void wr_syslog_start(bool enabled, const char *host, uint16_t port, uint8_t facility)
{
    /* (Re)close any existing socket first */
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }

    s_facility = facility;

    if (!enabled) {
        printf("I wr_syslog: disabled\n");
        return;
    }
    if (port == 0) port = SYSLOG_PORT_DEFAULT;

    bool broadcast = (!host || host[0] == '\0');
    uint32_t dest_addr;

    if (broadcast) {
        /* Compute directed broadcast from the active interface */
        dest_addr = INADDR_BROADCAST;  /* 255.255.255.255 fallback */
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!netif) netif  = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (netif) {
            esp_netif_ip_info_t info;
            if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr)
                dest_addr = info.ip.addr | ~info.netmask.addr;
        }
    } else {
        dest_addr = inet_addr(host);
        if (dest_addr == IPADDR_NONE) {
            printf("I wr_syslog: invalid host '%s', not started\n", host);
            return;
        }
    }

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return;

    if (broadcast) {
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    }

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = dest_addr,
    };
    if (connect(fd, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        close(fd);
        return;
    }

    s_sock = fd;

    char ip_str[16];
    inet_ntoa_r(*(struct in_addr *)&dest_addr, ip_str, sizeof(ip_str));
    /* use vprintf directly — avoid re-entrant ESP_LOGI through our hook */
    printf("I wr_syslog: forwarding logs to %s:%d (facility=%d)\n", ip_str, port, facility);
}
