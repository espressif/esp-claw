/**
 * @file cap_cron.h
 * @brief Cron Scheduler Capability for Neon-Claw
 * 
 * Implements cron-like scheduling for periodic tasks
 * Supports: */5 * * * *, @hourly, @daily, one-shot, recurring
 * 
 * @author maruf009sultan
 * @copyright Neon-Claw Project
 */

#pragma once

#include "esp_err.h"
#include "stdint.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRON_MAX_JOBS 32
#define CRON_EXPR_MAX_LEN 64

typedef enum {
    CRON_JOB_TYPE_ONESHOT = 0,
    CRON_JOB_TYPE_RECURRING,
    CRON_JOB_TYPE_CRON_EXPR
} cron_job_type_t;

typedef void (*cron_callback_t)(void *arg);

typedef struct {
    int id;
    char name[32];
    cron_job_type_t type;
    char cron_expr[CRON_EXPR_MAX_LEN];
    uint32_t interval_sec;
    uint64_t next_run;
    bool enabled;
    cron_callback_t callback;
    void *cb_arg;
} cron_job_t;

esp_err_t cap_cron_init(void);
esp_err_t cap_cron_deinit(void);

int cap_cron_schedule_oneshot(uint32_t delay_sec, cron_callback_t cb, void *arg);
int cap_cron_schedule_recurring(uint32_t interval_sec, cron_callback_t cb, void *arg);
int cap_cron_schedule_cron(const char *expr, cron_callback_t cb, void *arg);

esp_err_t cap_cron_cancel(int job_id);
esp_err_t cap_cron_enable(int job_id, bool enable);
esp_err_t cap_cron_get_job(int job_id, cron_job_t *out_job);
int cap_cron_get_active_count(void);

#ifdef __cplusplus
}
#endif
