/**
 * @file cap_cron.c
 * @brief Cron Scheduler Implementation for Neon-Claw
 * 
 * @author maruf009sultan
 * @copyright Neon-Claw Project
 */

#include "cap_cron.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "string.h"
#include "stdlib.h"

static const char *TAG = "cap_cron";

static cron_job_t s_jobs[CRON_MAX_JOBS];
static bool s_initialized = false;
static int s_next_id = 1;
static esp_timer_handle_t s_scheduler_timer;

static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void scheduler_callback(void *arg) {
    uint64_t now = get_time_ms();
    
    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        if (s_jobs[i].id > 0 && s_jobs[i].enabled && s_jobs[i].next_run <= now) {
            ESP_LOGD(TAG, "Running job %d: %s", s_jobs[i].id, s_jobs[i].name);
            
            if (s_jobs[i].callback) {
                s_jobs[i].callback(s_jobs[i].cb_arg);
            }
            
            if (s_jobs[i].type == CRON_JOB_TYPE_ONESHOT) {
                s_jobs[i].id = 0;  // Remove job
            } else {
                s_jobs[i].next_run = now + (s_jobs[i].interval_sec * 1000);
            }
        }
    }
}

esp_err_t cap_cron_init(void) {
    if (s_initialized) return ESP_OK;
    
    memset(s_jobs, 0, sizeof(s_jobs));
    s_next_id = 1;
    
    esp_timer_create_args_t timer_args = {
        .callback = scheduler_callback,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "cron_scheduler",
        .skip_unhandled_events = false
    };
    
    esp_err_t err = esp_timer_create(&timer_args, &s_scheduler_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create scheduler timer");
        return err;
    }
    
    err = esp_timer_start_periodic(s_scheduler_timer, 1000000);  // 1 second
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scheduler timer");
        esp_timer_delete(s_scheduler_timer);
        return err;
    }
    
    s_initialized = true;
    ESP_LOGI(TAG, "Cron scheduler initialized");
    return ESP_OK;
}

esp_err_t cap_cron_deinit(void) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    esp_timer_stop(s_scheduler_timer);
    esp_timer_delete(s_scheduler_timer);
    memset(s_jobs, 0, sizeof(s_jobs));
    s_initialized = false;
    
    ESP_LOGI(TAG, "Cron scheduler deinitialized");
    return ESP_OK;
}

int cap_cron_schedule_oneshot(uint32_t delay_sec, cron_callback_t cb, void *arg) {
    if (!s_initialized || cb == NULL) return -1;
    
    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        if (s_jobs[i].id == 0) {
            s_jobs[i].id = s_next_id++;
            snprintf(s_jobs[i].name, sizeof(s_jobs[i].name), "oneshot_%d", s_jobs[i].id);
            s_jobs[i].type = CRON_JOB_TYPE_ONESHOT;
            s_jobs[i].interval_sec = delay_sec;
            s_jobs[i].next_run = get_time_ms() + (delay_sec * 1000);
            s_jobs[i].enabled = true;
            s_jobs[i].callback = cb;
            s_jobs[i].cb_arg = arg;
            
            ESP_LOGI(TAG, "Scheduled oneshot job %d in %u sec", s_jobs[i].id, delay_sec);
            return s_jobs[i].id;
        }
    }
    
    ESP_LOGW(TAG, "No free job slots");
    return -1;
}

int cap_cron_schedule_recurring(uint32_t interval_sec, cron_callback_t cb, void *arg) {
    if (!s_initialized || cb == NULL || interval_sec == 0) return -1;
    
    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        if (s_jobs[i].id == 0) {
            s_jobs[i].id = s_next_id++;
            snprintf(s_jobs[i].name, sizeof(s_jobs[i].name), "recurring_%d", s_jobs[i].id);
            s_jobs[i].type = CRON_JOB_TYPE_RECURRING;
            s_jobs[i].interval_sec = interval_sec;
            s_jobs[i].next_run = get_time_ms() + (interval_sec * 1000);
            s_jobs[i].enabled = true;
            s_jobs[i].callback = cb;
            s_jobs[i].cb_arg = arg;
            
            ESP_LOGI(TAG, "Scheduled recurring job %d every %u sec", s_jobs[i].id, interval_sec);
            return s_jobs[i].id;
        }
    }
    
    ESP_LOGW(TAG, "No free job slots");
    return -1;
}

int cap_cron_schedule_cron(const char *expr, cron_callback_t cb, void *arg) {
    // Simplified: support @hourly, @daily
    if (!s_initialized || cb == NULL || expr == NULL) return -1;
    
    uint32_t interval_sec = 0;
    if (strcmp(expr, "@hourly") == 0) {
        interval_sec = 3600;
    } else if (strcmp(expr, "@daily") == 0) {
        interval_sec = 86400;
    } else {
        ESP_LOGW(TAG, "Unsupported cron expression: %s", expr);
        return -1;
    }
    
    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        if (s_jobs[i].id == 0) {
            s_jobs[i].id = s_next_id++;
            snprintf(s_jobs[i].name, sizeof(s_jobs[i].name), "cron_%d", s_jobs[i].id);
            strncpy(s_jobs[i].cron_expr, expr, CRON_EXPR_MAX_LEN - 1);
            s_jobs[i].type = CRON_JOB_TYPE_CRON_EXPR;
            s_jobs[i].interval_sec = interval_sec;
            s_jobs[i].next_run = get_time_ms() + (interval_sec * 1000);
            s_jobs[i].enabled = true;
            s_jobs[i].callback = cb;
            s_jobs[i].cb_arg = arg;
            
            ESP_LOGI(TAG, "Scheduled cron job %d: %s", s_jobs[i].id, expr);
            return s_jobs[i].id;
        }
    }
    
    ESP_LOGW(TAG, "No free job slots");
    return -1;
}

esp_err_t cap_cron_cancel(int job_id) {
    if (!s_initialized || job_id <= 0) return ESP_ERR_INVALID_ARG;
    
    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        if (s_jobs[i].id == job_id) {
            s_jobs[i].id = 0;
            ESP_LOGI(TAG, "Cancelled job %d", job_id);
            return ESP_OK;
        }
    }
    
    return ESP_ERR_NOT_FOUND;
}

esp_err_t cap_cron_enable(int job_id, bool enable) {
    if (!s_initialized || job_id <= 0) return ESP_ERR_INVALID_ARG;
    
    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        if (s_jobs[i].id == job_id) {
            s_jobs[i].enabled = enable;
            ESP_LOGI(TAG, "Job %d %s", job_id, enable ? "enabled" : "disabled");
            return ESP_OK;
        }
    }
    
    return ESP_ERR_NOT_FOUND;
}

esp_err_t cap_cron_get_job(int job_id, cron_job_t *out_job) {
    if (!s_initialized || job_id <= 0 || out_job == NULL) return ESP_ERR_INVALID_ARG;
    
    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        if (s_jobs[i].id == job_id) {
            memcpy(out_job, &s_jobs[i], sizeof(cron_job_t));
            return ESP_OK;
        }
    }
    
    return ESP_ERR_NOT_FOUND;
}

int cap_cron_get_active_count(void) {
    if (!s_initialized) return 0;
    
    int count = 0;
    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        if (s_jobs[i].id > 0 && s_jobs[i].enabled) {
            count++;
        }
    }
    return count;
}
