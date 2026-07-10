/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_scheduler_internal.h"

#include <ctype.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static bool cap_scheduler_parse_cron_field(const char *token,
                                           int min_value,
                                           int max_value,
                                           bool *allowed)
{
    int value;
    int step;

    if (!token || !token[0] || !allowed) {
        return false;
    }

    if (strcmp(token, "*") == 0) {
        for (value = min_value; value <= max_value; value++) {
            allowed[value] = true;
        }
        return true;
    }

    if (strncmp(token, "*/", 2) == 0) {
        step = atoi(token + 2);
        if (step <= 0) {
            return false;
        }
        for (value = min_value; value <= max_value; value++) {
            if (((value - min_value) % step) == 0) {
                allowed[value] = true;
            }
        }
        return true;
    }

    for (const char *p = token; *p; ++p) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }

    value = atoi(token);
    if (value < min_value || value > max_value) {
        return false;
    }
    allowed[value] = true;
    return true;
}

typedef struct {
    bool minute[60];
    bool hour[24];
    bool mday[32];
    bool month[13];
    bool wday[7];
} cap_scheduler_cron_spec_t;

static bool cap_scheduler_parse_cron_expr(const char *expr, cap_scheduler_cron_spec_t *out)
{
    char buf[CAP_SCHEDULER_EXPR_LEN];
    char *save = NULL;
    char *token = NULL;
    const int mins[5][2] = {
        {0, 59},
        {0, 23},
        {1, 31},
        {1, 12},
        {0, 6},
    };
    bool *fields[5] = {
        out->minute,
        out->hour,
        out->mday,
        out->month,
        out->wday,
    };
    int index = 0;

    if (!expr || !expr[0] || !out) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(buf, expr, sizeof(buf));

    token = strtok_r(buf, " \t", &save);
    while (token && index < 5) {
        if (!cap_scheduler_parse_cron_field(token, mins[index][0], mins[index][1], fields[index])) {
            return false;
        }
        index++;
        token = strtok_r(NULL, " \t", &save);
    }

    return index == 5 && token == NULL;
}

static bool cap_scheduler_compute_next_cron_fire(const cap_scheduler_item_t *item,
                                                 int64_t anchor_ms,
                                                 int64_t *out_next_fire_ms)
{
    cap_scheduler_cron_spec_t spec;
    int64_t candidate_ms;
    int i;

    if (!item || !out_next_fire_ms || !cap_scheduler_parse_cron_expr(item->cron_expr, &spec)) {
        return false;
    }

    candidate_ms = (anchor_ms / 60000LL) * 60000LL + 60000LL;
    for (i = 0; i < 525600; i++) {
        time_t seconds = (time_t)(candidate_ms / 1000LL);
        struct tm tm_info = {0};

        localtime_r(&seconds, &tm_info);

        if (spec.minute[tm_info.tm_min] &&
            spec.hour[tm_info.tm_hour] &&
            spec.mday[tm_info.tm_mday] &&
            spec.month[tm_info.tm_mon + 1] &&
            spec.wday[tm_info.tm_wday]) {
            *out_next_fire_ms = candidate_ms;
            return true;
        }
        candidate_ms += 60000LL;
    }

    return false;
}

int64_t cap_scheduler_now_ms(void)
{
    struct timeval tv = {0};

    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000LL) + (tv.tv_usec / 1000LL);
}

void cap_scheduler_apply_defaults(cap_scheduler_item_t *item)
{
    if (!item) {
        return;
    }

    if (!item->event_type[0]) {
        strlcpy(item->event_type, "schedule", sizeof(item->event_type));
    }
    if (!item->source_channel[0]) {
        strlcpy(item->source_channel, "time", sizeof(item->source_channel));
    }
    if (!item->content_type[0]) {
        strlcpy(item->content_type, "trigger", sizeof(item->content_type));
    }
    if (!item->session_policy[0]) {
        strlcpy(item->session_policy, "trigger", sizeof(item->session_policy));
    }
    if (!item->payload_json[0]) {
        strlcpy(item->payload_json, "{}", sizeof(item->payload_json));
    }
    if (!item->event_key[0]) {
        strlcpy(item->event_key, item->id, sizeof(item->event_key));
    }
}

esp_err_t cap_scheduler_validate_item(const cap_scheduler_item_t *item)
{
    if (!item || !item->id[0] || !item->event_key[0] || !item->event_type[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (item->kind) {
    case CAP_SCHEDULER_ITEM_ONCE:
        if (item->start_at_ms <= 0) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case CAP_SCHEDULER_ITEM_INTERVAL:
        if (item->interval_ms <= 0) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case CAP_SCHEDULER_ITEM_CRON:
        if (!item->cron_expr[0]) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t cap_scheduler_compute_next_fire(const cap_scheduler_item_t *item,
                                          int64_t anchor_ms,
                                          int run_count,
                                          int64_t *out_next_fire_ms)
{
    int64_t next_fire_ms = -1;

    if (!item || !out_next_fire_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!item->enabled) {
        *out_next_fire_ms = -1;
        return ESP_OK;
    }
    if (item->max_runs > 0 && run_count >= item->max_runs) {
        *out_next_fire_ms = -1;
        return ESP_OK;
    }

    switch (item->kind) {
    case CAP_SCHEDULER_ITEM_ONCE:
        next_fire_ms = item->start_at_ms;
        if (anchor_ms >= next_fire_ms) {
            next_fire_ms = -1;
        }
        break;
    case CAP_SCHEDULER_ITEM_INTERVAL:
        if (item->start_at_ms > 0) {
            next_fire_ms = item->start_at_ms;
            while (next_fire_ms <= anchor_ms) {
                next_fire_ms += item->interval_ms;
            }
        } else {
            next_fire_ms = anchor_ms + item->interval_ms;
        }
        break;
    case CAP_SCHEDULER_ITEM_CRON:
        if (!cap_scheduler_compute_next_cron_fire(item, anchor_ms, &next_fire_ms)) {
            return ESP_FAIL;
        }
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (next_fire_ms > 0 && item->end_at_ms > 0 && next_fire_ms > item->end_at_ms) {
        next_fire_ms = -1;
    }

    *out_next_fire_ms = next_fire_ms;
    return ESP_OK;
}
