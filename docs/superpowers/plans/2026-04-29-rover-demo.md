# rover_demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a new `application/rover_demo/` for esp-claw that runs on M5StickC Plus + RoverC Pro + UnitV-M12, with Telegram as the primary IM interface.

**Architecture:** Two new capability components (`cap_rover` for I2C motor/servo control, `cap_unitv` for UART camera + vision LLM) and a new application that wires them together with cap_im_tg. M5Unified provides the board abstraction (display, IMU, buttons, power) via a C wrapper.

**Tech Stack:** ESP-IDF v5.5.4, FreeRTOS, M5Unified (C++ via C wrapper), cJSON, mbedTLS, esp_http_client, native ESP-IDF I2C master + UART drivers.

---

## File Structure

### New component: `cap_rover`

```
components/claw_capabilities/cap_rover/
  CMakeLists.txt
  include/cap_rover.h            — public API: init, register_group, emergency_stop_set
  src/
    cap_rover_internal.h         — internal types: action req/result, queue handles
    cap_rover_hw.c               — rover_hw_task, I2C writes, action queue
    cap_rover.c                  — capability descriptors and execute functions
    cmd_cap_rover.c              — console commands for manual hardware testing
  skills/
    cap_rover.md
    skills_list.json
```

### New component: `cap_unitv`

```
components/claw_capabilities/cap_unitv/
  CMakeLists.txt
  include/cap_unitv.h            — public API: init, register_group, set_vision_config
  src/
    cap_unitv_internal.h         — internal types and shared state
    cap_unitv_uart.c             — UART init, send/recv JSON commands, JPEG read
    cap_unitv_capture.c          — JPEG capture + base64 + vision LLM HTTP call
    cap_unitv.c                  — capability descriptors and execute functions
    cmd_cap_unitv.c              — console commands
  skills/
    cap_unitv.md
    skills_list.json
```

### New application: `application/rover_demo/`

```
application/rover_demo/
  CMakeLists.txt
  sdkconfig.defaults
  partitions.csv
  idf_component.yml
  boards/m5stickc_plus/
    board_info.yaml
    board_peripherals.yaml
    board_devices.yaml
    sdkconfig.defaults.board
    setup_device.h               — C API exported by setup_device.cpp
    setup_device.cpp             — M5.begin(), display/IMU/battery wrappers
  fatfs_image/
    memory/
      identity.md
      soul.md
      user.md
      MEMORY.md
      memory_records.jsonl
      memory_index.json
      memory_digest.log
    router_rules/router_rules.json
    skills/
      skills_list.json
      rover_ops.md
      rover_search.md
  main/
    CMakeLists.txt
    idf_component.yml
    Kconfig.projbuild
    main.c                       — app_main entry
    rover_demo_settings.h/.c
    rover_demo_wifi.h/.c
    rover_display.h/.cpp
    rover_buttons.h/.c
    app_rover.h/.c               — wires everything (analog of basic_demo's app_claw.c)
```

---

## Task 1: cap_rover — Skeleton, headers, hardware task

**Files:**
- Create: `components/claw_capabilities/cap_rover/CMakeLists.txt`
- Create: `components/claw_capabilities/cap_rover/include/cap_rover.h`
- Create: `components/claw_capabilities/cap_rover/src/cap_rover_internal.h`
- Create: `components/claw_capabilities/cap_rover/src/cap_rover_hw.c`

- [ ] **Step 1: Create `CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS
        "src/cap_rover.c"
        "src/cap_rover_hw.c"
        "src/cmd_cap_rover.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        claw_cap
        claw_core
        driver
        console
        json
)
```

- [ ] **Step 2: Create `include/cap_rover.h`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "claw_cap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int i2c_port;                  /* e.g., I2C_NUM_1 */
    int sda_gpio;                  /* GPIO0 on M5StickC Plus HAT */
    int scl_gpio;                  /* GPIO26 */
    uint32_t i2c_freq_hz;          /* 100000 */
    uint8_t rover_addr;            /* 0x38 */
    uint8_t gripper_servo_idx;     /* 1 (servo position 1) */
    uint8_t gripper_open_angle;    /* 35 */
    uint8_t gripper_close_angle;   /* 150 */
    uint32_t hw_task_stack_size;   /* 4096 */
    UBaseType_t hw_task_priority;  /* 5 */
    BaseType_t hw_task_core;       /* tskNO_AFFINITY or core 0 */
} cap_rover_config_t;

typedef esp_err_t (*cap_rover_imu_read_fn)(float *ax, float *ay, float *az,
                                           float *gx, float *gy, float *gz);

esp_err_t cap_rover_init(const cap_rover_config_t *config);
esp_err_t cap_rover_register_group(void);

/* Set the IMU read function used by rover_turn (gyro feedback) and rover_read_imu.
 * Pass NULL to disable IMU-dependent capabilities. */
void cap_rover_set_imu_read(cap_rover_imu_read_fn fn);

/* Set the emergency stop flag. Safe to call from any context including ISR
 * (uses simple bool write — atomic on Xtensa). The hardware task picks up
 * the flag at its next 50ms tick and aborts the active action. */
void cap_rover_emergency_stop_set(void);

/* Clear the flag. Called by the hardware task once an emergency stop has been
 * processed, so the next user command can proceed. */
void cap_rover_emergency_stop_clear(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Create `src/cap_rover_internal.h`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "cap_rover.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROVER_ACTION_MOVE = 1,
    ROVER_ACTION_TURN = 2,
    ROVER_ACTION_STOP = 3,
    ROVER_ACTION_GRIPPER_OPEN = 4,
    ROVER_ACTION_GRIPPER_CLOSE = 5,
} rover_action_kind_t;

typedef struct {
    uint32_t req_id;
    rover_action_kind_t kind;
    int8_t x;
    int8_t y;
    int8_t z;
    uint16_t duration_ms;
    uint16_t turn_target_deg;
    uint16_t turn_timeout_ms;
} rover_action_req_t;

typedef struct {
    uint32_t req_id;
    esp_err_t err;
    float turn_measured_deg;
} rover_action_result_t;

typedef struct {
    cap_rover_config_t cfg;
    i2c_master_bus_handle_t i2c_bus;
    i2c_device_handle_t i2c_dev;
    QueueHandle_t req_queue;
    QueueHandle_t result_queue;
    SemaphoreHandle_t queue_lock;
    TaskHandle_t hw_task;
    cap_rover_imu_read_fn imu_read;
    volatile bool emergency_requested;
    atomic_uint_fast32_t req_seq;
    bool initialized;
} cap_rover_state_t;

extern cap_rover_state_t g_cap_rover;

/* Submit a request to the hw task and block until it completes or timeout.
 * On success, fills *out_result. */
esp_err_t cap_rover_submit_and_wait(rover_action_req_t *req,
                                    TickType_t timeout,
                                    rover_action_result_t *out_result);

/* Low-level I2C operations (called by hw task only). */
esp_err_t cap_rover_hw_set_speed(int8_t x, int8_t y, int8_t z);
esp_err_t cap_rover_hw_set_servo_angle(uint8_t servo_idx, uint8_t angle);
void      cap_rover_hw_zero_motors(void);

/* Start the hardware task. Called from cap_rover_init. */
esp_err_t cap_rover_hw_start(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 4: Create `src/cap_rover_hw.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_rover_internal.h"

#include <math.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "cap_rover_hw";

cap_rover_state_t g_cap_rover = {0};

#define ROVER_REG_MOTOR_BASE   0x00
#define ROVER_REG_SERVO_ANGLE  0x10
#define ROVER_HW_TICK_MS       50
#define ROVER_GYRO_DEAD_BAND_DPS 3.0f
#define ROVER_RESULT_QUEUE_DEPTH 4
#define ROVER_REQ_QUEUE_DEPTH    4

static int8_t clamp_speed(int32_t v)
{
    if (v > 100)  return 100;
    if (v < -100) return -100;
    return (int8_t)v;
}

static esp_err_t i2c_write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    if (!g_cap_rover.i2c_dev) return ESP_ERR_INVALID_STATE;
    if (len + 1 > 8) return ESP_ERR_INVALID_ARG;
    uint8_t buf[8];
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(g_cap_rover.i2c_dev, buf, len + 1, pdMS_TO_TICKS(50));
}

esp_err_t cap_rover_hw_set_speed(int8_t x, int8_t y, int8_t z)
{
    /* Negate z to match RoverC's hardware rotation convention. */
    int32_t zn = -z;
    int32_t xa = x, ya = y;
    if (zn != 0) {
        int32_t scale = 100 - (zn > 0 ? zn : -zn);
        xa = (xa * scale) / 100;
        ya = (ya * scale) / 100;
    }
    int8_t buf[4] = {
        clamp_speed(ya + xa - zn),
        clamp_speed(ya - xa + zn),
        clamp_speed(ya - xa - zn),
        clamp_speed(ya + xa + zn),
    };
    return i2c_write_reg(ROVER_REG_MOTOR_BASE, (const uint8_t *)buf, sizeof(buf));
}

esp_err_t cap_rover_hw_set_servo_angle(uint8_t servo_idx, uint8_t angle)
{
    uint8_t reg = (uint8_t)(ROVER_REG_SERVO_ANGLE + servo_idx);
    return i2c_write_reg(reg, &angle, 1);
}

void cap_rover_hw_zero_motors(void)
{
    int8_t zero[4] = {0, 0, 0, 0};
    (void)i2c_write_reg(ROVER_REG_MOTOR_BASE, (const uint8_t *)zero, sizeof(zero));
}

void cap_rover_emergency_stop_set(void)
{
    g_cap_rover.emergency_requested = true;
}

void cap_rover_emergency_stop_clear(void)
{
    g_cap_rover.emergency_requested = false;
}

void cap_rover_set_imu_read(cap_rover_imu_read_fn fn)
{
    g_cap_rover.imu_read = fn;
}

static void send_result(uint32_t req_id, esp_err_t err, float measured_deg)
{
    rover_action_result_t r = {
        .req_id = req_id,
        .err = err,
        .turn_measured_deg = measured_deg,
    };
    /* Drop oldest if queue is full so the latest result always lands. */
    if (xQueueSend(g_cap_rover.result_queue, &r, 0) != pdTRUE) {
        rover_action_result_t dropped;
        (void)xQueueReceive(g_cap_rover.result_queue, &dropped, 0);
        (void)xQueueSend(g_cap_rover.result_queue, &r, 0);
    }
}

static esp_err_t execute_move(const rover_action_req_t *req, float *measured_deg)
{
    *measured_deg = 0.0f;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(req->duration_ms);
    esp_err_t err = ESP_OK;

    while ((int32_t)(end - xTaskGetTickCount()) > 0) {
        if (g_cap_rover.emergency_requested) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        esp_err_t e = cap_rover_hw_set_speed(req->x, req->y, req->z);
        if (err == ESP_OK && e != ESP_OK) err = e;
        vTaskDelay(pdMS_TO_TICKS(ROVER_HW_TICK_MS));
    }
    cap_rover_hw_zero_motors();
    return err;
}

static esp_err_t execute_turn(const rover_action_req_t *req, float *measured_deg)
{
    *measured_deg = 0.0f;
    if (!g_cap_rover.imu_read) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    float target = (float)req->turn_target_deg;
    TickType_t start_tick = xTaskGetTickCount();
    uint32_t prev_ms = (uint32_t)(esp_log_timestamp());
    esp_err_t err = ESP_OK;
    float turned = 0.0f;

    while (turned < target &&
           (xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(req->turn_timeout_ms)) {
        if (g_cap_rover.emergency_requested) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        float ax, ay, az, gx, gy, gz;
        if (g_cap_rover.imu_read(&ax, &ay, &az, &gx, &gy, &gz) != ESP_OK) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        uint32_t now_ms = (uint32_t)(esp_log_timestamp());
        float dt = (float)(now_ms - prev_ms) / 1000.0f;
        prev_ms = now_ms;

        esp_err_t e = cap_rover_hw_set_speed(0, 0, req->z);
        if (err == ESP_OK && e != ESP_OK) err = e;

        float rate = fabsf(gx);
        if (fabsf(gy) > rate) rate = fabsf(gy);
        if (fabsf(gz) > rate) rate = fabsf(gz);
        if (rate > ROVER_GYRO_DEAD_BAND_DPS) {
            turned += rate * dt;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    cap_rover_hw_zero_motors();
    *measured_deg = turned;
    if (err == ESP_OK && turned < target) err = ESP_ERR_TIMEOUT;
    return err;
}

static void hw_task(void *arg)
{
    (void)arg;
    rover_action_req_t req;
    while (1) {
        if (xQueueReceive(g_cap_rover.req_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ESP_LOGI(TAG, "hw_task action kind=%d req_id=%u", (int)req.kind, (unsigned)req.req_id);

        esp_err_t err = ESP_OK;
        float measured_deg = 0.0f;

        switch (req.kind) {
        case ROVER_ACTION_MOVE:
            err = execute_move(&req, &measured_deg);
            break;
        case ROVER_ACTION_TURN:
            err = execute_turn(&req, &measured_deg);
            break;
        case ROVER_ACTION_STOP:
            cap_rover_hw_zero_motors();
            err = ESP_OK;
            break;
        case ROVER_ACTION_GRIPPER_OPEN:
            err = cap_rover_hw_set_servo_angle(g_cap_rover.cfg.gripper_servo_idx,
                                               g_cap_rover.cfg.gripper_open_angle);
            break;
        case ROVER_ACTION_GRIPPER_CLOSE:
            err = cap_rover_hw_set_servo_angle(g_cap_rover.cfg.gripper_servo_idx,
                                               g_cap_rover.cfg.gripper_close_angle);
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
        }

        if (err == ESP_ERR_INVALID_STATE) {
            /* Emergency stop fired — clear so the next command starts clean. */
            cap_rover_emergency_stop_clear();
        }
        send_result(req.req_id, err, measured_deg);
    }
}

esp_err_t cap_rover_hw_start(void)
{
    g_cap_rover.req_queue = xQueueCreate(ROVER_REQ_QUEUE_DEPTH, sizeof(rover_action_req_t));
    g_cap_rover.result_queue = xQueueCreate(ROVER_RESULT_QUEUE_DEPTH, sizeof(rover_action_result_t));
    g_cap_rover.queue_lock = xSemaphoreCreateMutex();
    if (!g_cap_rover.req_queue || !g_cap_rover.result_queue || !g_cap_rover.queue_lock) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(hw_task, "rover_hw",
                                            g_cap_rover.cfg.hw_task_stack_size,
                                            NULL,
                                            g_cap_rover.cfg.hw_task_priority,
                                            &g_cap_rover.hw_task,
                                            g_cap_rover.cfg.hw_task_core);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t cap_rover_submit_and_wait(rover_action_req_t *req,
                                    TickType_t timeout,
                                    rover_action_result_t *out_result)
{
    if (!req || !g_cap_rover.req_queue || !g_cap_rover.result_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    req->req_id = (uint32_t)atomic_fetch_add(&g_cap_rover.req_seq, 1) + 1;

    /* Lock prevents two callers from racing on the queues. The hw task itself
     * does not take this lock — it only reads from req_queue. */
    xSemaphoreTake(g_cap_rover.queue_lock, portMAX_DELAY);
    BaseType_t sent = xQueueSend(g_cap_rover.req_queue, req, pdMS_TO_TICKS(100));
    if (sent != pdTRUE) {
        xSemaphoreGive(g_cap_rover.queue_lock);
        return ESP_ERR_TIMEOUT;
    }

    /* Wait for the matching result. Skip stale results from earlier callers. */
    TickType_t deadline = xTaskGetTickCount() + timeout;
    esp_err_t final_err = ESP_ERR_TIMEOUT;
    while (1) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) break;
        rover_action_result_t r;
        if (xQueueReceive(g_cap_rover.result_queue, &r, deadline - now) != pdTRUE) break;
        if (r.req_id == req->req_id) {
            if (out_result) *out_result = r;
            final_err = r.err;
            break;
        }
        /* Stale; drop and keep waiting. */
    }
    xSemaphoreGive(g_cap_rover.queue_lock);
    return final_err;
}

esp_err_t cap_rover_init(const cap_rover_config_t *config)
{
    if (g_cap_rover.initialized) return ESP_OK;
    if (!config) return ESP_ERR_INVALID_ARG;

    g_cap_rover.cfg = *config;
    atomic_init(&g_cap_rover.req_seq, 0);
    g_cap_rover.emergency_requested = false;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = config->i2c_port,
        .sda_io_num = config->sda_gpio,
        .scl_io_num = config->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &g_cap_rover.i2c_bus),
                        TAG, "i2c_new_master_bus failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->rover_addr,
        .scl_speed_hz = config->i2c_freq_hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(g_cap_rover.i2c_bus, &dev_cfg, &g_cap_rover.i2c_dev),
                        TAG, "i2c_master_bus_add_device failed");

    /* Best-effort zero motors at boot. RoverC may not be powered yet, so log only. */
    esp_err_t e = cap_rover_hw_set_speed(0, 0, 0);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "Initial motor zero failed: %s (RoverC not powered?)", esp_err_to_name(e));
    }

    ESP_RETURN_ON_ERROR(cap_rover_hw_start(), TAG, "hw_start failed");
    g_cap_rover.initialized = true;
    ESP_LOGI(TAG, "cap_rover initialized (i2c port=%d sda=%d scl=%d addr=0x%02x)",
             config->i2c_port, config->sda_gpio, config->scl_gpio, config->rover_addr);
    return ESP_OK;
}
```

- [ ] **Step 5: Build cap_rover (will fail until cap_rover.c exists in next task)**

This step verifies the headers parse and CMake registration is valid. Defer the actual idf.py build until after Task 2.

- [ ] **Step 6: Commit**

```bash
git add components/claw_capabilities/cap_rover/CMakeLists.txt \
        components/claw_capabilities/cap_rover/include/cap_rover.h \
        components/claw_capabilities/cap_rover/src/cap_rover_internal.h \
        components/claw_capabilities/cap_rover/src/cap_rover_hw.c
git commit -m "feat(cap_rover): add I2C hardware task and action queue"
```

---

## Task 2: cap_rover — Capability registration and execute functions

**Files:**
- Create: `components/claw_capabilities/cap_rover/src/cap_rover.c`

- [ ] **Step 1: Create `src/cap_rover.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_rover_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "cap_rover";

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static const char *err_to_status(esp_err_t err)
{
    if (err == ESP_OK) return "ok";
    if (err == ESP_ERR_INVALID_STATE) return "emergency_stop";
    if (err == ESP_ERR_TIMEOUT) return "timeout";
    if (err == ESP_ERR_NOT_SUPPORTED) return "imu_unavailable";
    return "failed";
}

static esp_err_t cap_rover_move_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)ctx;
    if (!output || output_size == 0) return ESP_ERR_INVALID_ARG;
    int x = 0, y = 0, z = 0, duration_ms = 1500;
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    if (args) {
        cJSON *v;
        v = cJSON_GetObjectItem(args, "x"); if (v && cJSON_IsNumber(v)) x = v->valueint;
        v = cJSON_GetObjectItem(args, "y"); if (v && cJSON_IsNumber(v)) y = v->valueint;
        v = cJSON_GetObjectItem(args, "z"); if (v && cJSON_IsNumber(v)) z = v->valueint;
        v = cJSON_GetObjectItem(args, "duration_ms");
        if (v && cJSON_IsNumber(v)) duration_ms = v->valueint;
        cJSON_Delete(args);
    }
    x = clamp_int(x, -100, 100);
    y = clamp_int(y, -100, 100);
    z = clamp_int(z, -100, 100);
    duration_ms = clamp_int(duration_ms, 100, 5000);

    rover_action_req_t req = {
        .kind = ROVER_ACTION_MOVE,
        .x = (int8_t)x, .y = (int8_t)y, .z = (int8_t)z,
        .duration_ms = (uint16_t)duration_ms,
    };
    rover_action_result_t result = {0};
    TickType_t timeout = pdMS_TO_TICKS(duration_ms + 1000);
    esp_err_t err = cap_rover_submit_and_wait(&req, timeout, &result);

    snprintf(output, output_size,
             "{\"status\":\"%s\",\"action\":\"rover_move\",\"x\":%d,\"y\":%d,\"z\":%d,\"duration_ms\":%d}",
             err_to_status(err), x, y, z, duration_ms);
    return ESP_OK; /* Tool result is informational; non-OK encoded in status field. */
}

static esp_err_t cap_rover_turn_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)ctx;
    if (!output || output_size == 0) return ESP_ERR_INVALID_ARG;

    char dir[8] = "left";
    int angle_deg = 90, speed_pct = 50;
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    if (args) {
        cJSON *v;
        v = cJSON_GetObjectItem(args, "direction");
        if (v && cJSON_IsString(v) && v->valuestring) {
            strlcpy(dir, v->valuestring, sizeof(dir));
        }
        v = cJSON_GetObjectItem(args, "angle_deg"); if (v && cJSON_IsNumber(v)) angle_deg = v->valueint;
        v = cJSON_GetObjectItem(args, "speed_percent"); if (v && cJSON_IsNumber(v)) speed_pct = v->valueint;
        cJSON_Delete(args);
    }

    bool turn_left = (strcmp(dir, "right") != 0);
    int target = clamp_int(angle_deg, 5, 360);
    int spd = clamp_int(speed_pct, 20, 100);
    int8_t turn_z = (int8_t)(turn_left ? -spd : spd);
    uint32_t timeout_ms = (uint32_t)clamp_int(target * 100, 2000, 12000);

    rover_action_req_t req = {
        .kind = ROVER_ACTION_TURN,
        .x = 0, .y = 0, .z = turn_z,
        .turn_target_deg = (uint16_t)target,
        .turn_timeout_ms = (uint16_t)timeout_ms,
    };
    rover_action_result_t result = {0};
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(timeout_ms + 1000), &result);

    snprintf(output, output_size,
             "{\"status\":\"%s\",\"action\":\"rover_turn\",\"direction\":\"%s\","
             "\"target_deg\":%d,\"measured_deg\":%.1f}",
             err_to_status(err), turn_left ? "left" : "right",
             target, (double)result.turn_measured_deg);
    return ESP_OK;
}

static esp_err_t cap_rover_stop_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)input_json; (void)ctx;
    if (!output || output_size == 0) return ESP_ERR_INVALID_ARG;
    rover_action_req_t req = { .kind = ROVER_ACTION_STOP };
    rover_action_result_t result = {0};
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(2000), &result);
    snprintf(output, output_size,
             "{\"status\":\"%s\",\"action\":\"rover_stop\"}",
             err_to_status(err));
    return ESP_OK;
}

static esp_err_t gripper_execute(rover_action_kind_t kind, const char *action_label,
                                 char *output, size_t output_size)
{
    rover_action_req_t req = { .kind = kind };
    rover_action_result_t result = {0};
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(2000), &result);
    snprintf(output, output_size,
             "{\"status\":\"%s\",\"action\":\"%s\"}",
             err_to_status(err), action_label);
    return ESP_OK;
}

static esp_err_t cap_rover_gripper_open_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output, size_t output_size)
{
    (void)input_json; (void)ctx;
    if (!output || output_size == 0) return ESP_ERR_INVALID_ARG;
    return gripper_execute(ROVER_ACTION_GRIPPER_OPEN, "rover_gripper_open", output, output_size);
}

static esp_err_t cap_rover_gripper_close_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output, size_t output_size)
{
    (void)input_json; (void)ctx;
    if (!output || output_size == 0) return ESP_ERR_INVALID_ARG;
    return gripper_execute(ROVER_ACTION_GRIPPER_CLOSE, "rover_gripper_close", output, output_size);
}

static esp_err_t cap_rover_read_imu_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output, size_t output_size)
{
    (void)input_json; (void)ctx;
    if (!output || output_size == 0) return ESP_ERR_INVALID_ARG;
    if (!g_cap_rover.imu_read) {
        snprintf(output, output_size,
                 "{\"status\":\"imu_unavailable\",\"action\":\"rover_read_imu\"}");
        return ESP_OK;
    }
    float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
    esp_err_t err = g_cap_rover.imu_read(&ax, &ay, &az, &gx, &gy, &gz);
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "{\"status\":\"imu_read_failed\",\"action\":\"rover_read_imu\"}");
        return ESP_OK;
    }
    snprintf(output, output_size,
             "{\"status\":\"ok\",\"action\":\"rover_read_imu\","
             "\"accel\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
             "\"gyro\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}",
             (double)ax, (double)ay, (double)az,
             (double)gx, (double)gy, (double)gz);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_rover_descriptors[] = {
    {
        .id = "rover_move",
        .name = "rover_move",
        .family = "rover",
        .description = "Move the rover with a velocity vector for a duration, then stop.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
              "\"x\":{\"type\":\"integer\",\"minimum\":-100,\"maximum\":100,"
                    "\"description\":\"Lateral speed (left negative)\"},"
              "\"y\":{\"type\":\"integer\",\"minimum\":-100,\"maximum\":100,"
                    "\"description\":\"Forward speed (back negative)\"},"
              "\"z\":{\"type\":\"integer\",\"minimum\":-100,\"maximum\":100,"
                    "\"description\":\"Rotation speed (left negative)\"},"
              "\"duration_ms\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":5000,"
                    "\"description\":\"Duration in milliseconds\"}"
            "},\"required\":[\"x\",\"y\"]}",
        .execute = cap_rover_move_execute,
    },
    {
        .id = "rover_turn",
        .name = "rover_turn",
        .family = "rover",
        .description = "Rotate the rover in place by a given angle using IMU gyro feedback.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
              "\"direction\":{\"type\":\"string\",\"enum\":[\"left\",\"right\"]},"
              "\"angle_deg\":{\"type\":\"integer\",\"minimum\":5,\"maximum\":360},"
              "\"speed_percent\":{\"type\":\"integer\",\"minimum\":20,\"maximum\":100}"
            "},\"required\":[\"direction\"]}",
        .execute = cap_rover_turn_execute,
    },
    {
        .id = "rover_stop",
        .name = "rover_stop",
        .family = "rover",
        .description = "Stop all rover motion immediately.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_rover_stop_execute,
    },
    {
        .id = "rover_gripper_open",
        .name = "rover_gripper_open",
        .family = "rover",
        .description = "Open the rover's gripper.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_rover_gripper_open_execute,
    },
    {
        .id = "rover_gripper_close",
        .name = "rover_gripper_close",
        .family = "rover",
        .description = "Close the rover's gripper.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_rover_gripper_close_execute,
    },
    {
        .id = "rover_read_imu",
        .name = "rover_read_imu",
        .family = "rover",
        .description = "Read accelerometer and gyroscope values from the rover's IMU.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_rover_read_imu_execute,
    },
};

static const claw_cap_group_t s_rover_group = {
    .group_id = "cap_rover",
    .plugin_name = "cap_rover",
    .descriptors = s_rover_descriptors,
    .descriptor_count = sizeof(s_rover_descriptors) / sizeof(s_rover_descriptors[0]),
};

esp_err_t cap_rover_register_group(void)
{
    if (claw_cap_group_exists(s_rover_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_rover_group);
}
```

- [ ] **Step 2: Commit**

```bash
git add components/claw_capabilities/cap_rover/src/cap_rover.c
git commit -m "feat(cap_rover): add capability descriptors and execute functions"
```

---

## Task 3: cap_rover — CLI commands and skills

**Files:**
- Create: `components/claw_capabilities/cap_rover/src/cmd_cap_rover.c`
- Create: `components/claw_capabilities/cap_rover/skills/cap_rover.md`
- Create: `components/claw_capabilities/cap_rover/skills/skills_list.json`

- [ ] **Step 1: Create `src/cmd_cap_rover.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_log.h"
#include "cap_rover.h"
#include "cap_rover_internal.h"

static const char *TAG = "cmd_cap_rover";

static struct {
    struct arg_int *x;
    struct arg_int *y;
    struct arg_int *z;
    struct arg_int *duration_ms;
    struct arg_end *end;
} s_move_args;

static int cmd_rover_move(int argc, char **argv)
{
    int errs = arg_parse(argc, argv, (void **)&s_move_args);
    if (errs > 0) {
        arg_print_errors(stderr, s_move_args.end, argv[0]);
        return 1;
    }
    int x = s_move_args.x->count ? s_move_args.x->ival[0] : 0;
    int y = s_move_args.y->count ? s_move_args.y->ival[0] : 60;
    int z = s_move_args.z->count ? s_move_args.z->ival[0] : 0;
    int dur = s_move_args.duration_ms->count ? s_move_args.duration_ms->ival[0] : 1500;

    rover_action_req_t req = {
        .kind = ROVER_ACTION_MOVE,
        .x = (int8_t)x, .y = (int8_t)y, .z = (int8_t)z,
        .duration_ms = (uint16_t)dur,
    };
    rover_action_result_t r = {0};
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(dur + 1000), &r);
    printf("rover_move: err=%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_rover_stop(int argc, char **argv)
{
    (void)argc; (void)argv;
    rover_action_req_t req = { .kind = ROVER_ACTION_STOP };
    rover_action_result_t r = {0};
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(1000), &r);
    printf("rover_stop: err=%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_rover_open(int argc, char **argv)
{
    (void)argc; (void)argv;
    rover_action_req_t req = { .kind = ROVER_ACTION_GRIPPER_OPEN };
    rover_action_result_t r = {0};
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(1000), &r);
    printf("gripper_open: err=%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_rover_close(int argc, char **argv)
{
    (void)argc; (void)argv;
    rover_action_req_t req = { .kind = ROVER_ACTION_GRIPPER_CLOSE };
    rover_action_result_t r = {0};
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(1000), &r);
    printf("gripper_close: err=%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

void cap_rover_register_cli(void)
{
    s_move_args.x           = arg_int0("x", "x", "<int>", "lateral -100..100");
    s_move_args.y           = arg_int0("y", "y", "<int>", "forward -100..100");
    s_move_args.z           = arg_int0("z", "z", "<int>", "rotate -100..100");
    s_move_args.duration_ms = arg_int0("d", "duration", "<ms>", "duration in ms");
    s_move_args.end         = arg_end(2);

    const esp_console_cmd_t move_cmd = {
        .command = "rover_move",
        .help = "Move rover with velocity vector for a duration",
        .hint = NULL,
        .func = cmd_rover_move,
        .argtable = &s_move_args,
    };
    const esp_console_cmd_t stop_cmd = {
        .command = "rover_stop",
        .help = "Stop rover immediately",
        .func = cmd_rover_stop,
    };
    const esp_console_cmd_t open_cmd = {
        .command = "rover_open",
        .help = "Open gripper",
        .func = cmd_rover_open,
    };
    const esp_console_cmd_t close_cmd = {
        .command = "rover_close",
        .help = "Close gripper",
        .func = cmd_rover_close,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&move_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&open_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&close_cmd));
    ESP_LOGI(TAG, "cap_rover CLI commands registered");
}
```

Add to `cap_rover.h`:
```c
/* Register console commands. Optional — call after esp_console_init. */
void cap_rover_register_cli(void);
```

- [ ] **Step 2: Create `skills/cap_rover.md`**

```markdown
# Rover Operations

You control a four-wheeled mecanum rover with a gripper. The rover is a real
physical device — every action has consequences.

## Tools

### rover_move(x, y, z, duration_ms)

Move with a 3-axis velocity vector for `duration_ms`, then stop.
- `x`: lateral speed (-100..100). Negative = strafe left.
- `y`: forward speed (-100..100). Negative = backward.
- `z`: rotation speed (-100..100). Negative = rotate left.
- `duration_ms`: 100..5000.

Returns `{"status":"ok|emergency_stop|timeout|failed", ...}`.

### rover_turn(direction, angle_deg, speed_percent)

Rotate in place by `angle_deg` degrees using IMU gyro feedback. Stops when
the integrated yaw matches the target or the operation times out.
- `direction`: "left" or "right".
- `angle_deg`: 5..360.
- `speed_percent`: 20..100 (default 50).

Returns `{"status":..., "target_deg":N, "measured_deg":N.N}`.

### rover_stop()

Immediately zero all motors. Use when something looks wrong, when the user
asks to stop, or before changing direction sharply.

### rover_gripper_open() / rover_gripper_close()

Servo-controlled gripper. Use these when picking up or releasing objects.

### rover_read_imu()

Returns current accelerometer and gyroscope readings. Useful for verifying
the rover is level or detecting collisions (sudden accel spikes).

## Conventions

- Single move actions cap at 5 seconds. For longer trajectories, chain
  multiple `rover_move` calls — for each, observe the previous result before
  continuing.
- If a tool returns `"emergency_stop"`, the user pressed BtnB. Do not retry
  the action — acknowledge and ask what to do next.
- For precise rotations, prefer `rover_turn` over `rover_move(z=...)`. The
  IMU feedback compensates for motor variance.
- The camera is fixed front-facing. To look elsewhere, call `rover_turn`.
```

- [ ] **Step 3: Create `skills/skills_list.json`**

```json
{
  "skills": [
    {
      "id": "cap_rover",
      "file": "cap_rover.md",
      "summary": "Drive the mecanum rover: move, turn (with IMU), stop, gripper, IMU.",
      "cap_groups": ["cap_rover"]
    }
  ]
}
```

- [ ] **Step 4: Update `CMakeLists.txt` if needed (already lists `cmd_cap_rover.c`)**

Verify the file is in SRCS — already done in Task 1, Step 1.

- [ ] **Step 5: Commit**

```bash
git add components/claw_capabilities/cap_rover/src/cmd_cap_rover.c \
        components/claw_capabilities/cap_rover/include/cap_rover.h \
        components/claw_capabilities/cap_rover/skills/
git commit -m "feat(cap_rover): add CLI commands and skill document"
```

---

## Task 4: cap_unitv — Skeleton, headers, UART layer

**Files:**
- Create: `components/claw_capabilities/cap_unitv/CMakeLists.txt`
- Create: `components/claw_capabilities/cap_unitv/include/cap_unitv.h`
- Create: `components/claw_capabilities/cap_unitv/src/cap_unitv_internal.h`
- Create: `components/claw_capabilities/cap_unitv/src/cap_unitv_uart.c`

- [ ] **Step 1: Create `CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS
        "src/cap_unitv.c"
        "src/cap_unitv_uart.c"
        "src/cap_unitv_capture.c"
        "src/cmd_cap_unitv.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        claw_cap
        claw_core
        driver
        esp_http_client
        mbedtls
        console
        json
)
```

- [ ] **Step 2: Create `include/cap_unitv.h`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart_port;            /* UART_NUM_1 */
    int tx_gpio;              /* GPIO32 on M5StickC Plus Grove */
    int rx_gpio;              /* GPIO33 */
    int baud_rate;            /* 115200 */
    int rx_buffer_bytes;      /* 4096 */
    int default_timeout_ms;   /* 7000 */
    int capture_timeout_ms;   /* 12000 */
    int max_jpeg_bytes;       /* 40960 (K210 limit) */
} cap_unitv_config_t;

typedef struct {
    const char *api_key;
    const char *backend_type;   /* "openai_compatible" or "anthropic"; NULL => openai_compatible */
    const char *model;
    const char *base_url;       /* e.g., "https://openrouter.ai/api/v1" */
    const char *auth_type;      /* "bearer" (default) or "x-api-key" */
    uint32_t timeout_ms;
    uint32_t max_response_tokens;
} cap_unitv_vision_config_t;

esp_err_t cap_unitv_init(const cap_unitv_config_t *config);
esp_err_t cap_unitv_register_group(void);
void      cap_unitv_set_vision_config(const cap_unitv_vision_config_t *config);
void      cap_unitv_register_cli(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Create `src/cap_unitv_internal.h`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cap_unitv.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    cap_unitv_config_t cfg;
    cap_unitv_vision_config_t vision_cfg;
    char vision_api_key[192];
    char vision_model[64];
    char vision_base_url[128];
    char vision_backend_type[32];
    char vision_auth_type[16];
    bool vision_configured;
    SemaphoreHandle_t uart_mutex;
    atomic_uint_fast32_t req_seq;
    atomic_bool available;
    bool initialized;
} cap_unitv_state_t;

extern cap_unitv_state_t g_cap_unitv;

/* Send a JSON command and read one JSON line response. */
esp_err_t cap_unitv_uart_cmd(const char *cmd, const char *args_json,
                             char *resp, size_t resp_size, int timeout_ms);

/* Send CAPTURE command and receive a binary JPEG. Caller must free *jpeg_out. */
esp_err_t cap_unitv_uart_capture_jpeg(int quality, uint8_t **jpeg_out, size_t *jpeg_size_out);

/* Make a vision LLM call with a captured JPEG. Writes analysis JSON or text into
 * resp/resp_size. Uses g_cap_unitv.vision_cfg. Returns ESP_ERR_NOT_SUPPORTED if
 * the configured backend cannot handle images, ESP_ERR_INVALID_STATE if vision
 * config is missing. */
esp_err_t cap_unitv_vision_call(const char *question, const uint8_t *jpeg, size_t jpeg_size,
                                char *resp, size_t resp_size);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 4: Create `src/cap_unitv_uart.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_unitv_internal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "cap_unitv_uart";

cap_unitv_state_t g_cap_unitv = {0};

esp_err_t cap_unitv_init(const cap_unitv_config_t *config)
{
    if (g_cap_unitv.initialized) return ESP_OK;
    if (!config) return ESP_ERR_INVALID_ARG;

    g_cap_unitv.cfg = *config;
    atomic_init(&g_cap_unitv.req_seq, 0);
    atomic_init(&g_cap_unitv.available, false);
    g_cap_unitv.uart_mutex = xSemaphoreCreateMutex();
    if (!g_cap_unitv.uart_mutex) return ESP_ERR_NO_MEM;

    uart_config_t uart_cfg = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(config->uart_port,
                                            config->rx_buffer_bytes * 2,
                                            config->rx_buffer_bytes,
                                            0, NULL, 0),
                        TAG, "uart_driver_install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(config->uart_port, &uart_cfg),
                        TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(config->uart_port,
                                     config->tx_gpio, config->rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");
    g_cap_unitv.initialized = true;
    ESP_LOGI(TAG, "cap_unitv UART ready: port=%d tx=%d rx=%d baud=%d",
             config->uart_port, config->tx_gpio, config->rx_gpio, config->baud_rate);
    return ESP_OK;
}

void cap_unitv_set_vision_config(const cap_unitv_vision_config_t *config)
{
    if (!config) {
        g_cap_unitv.vision_configured = false;
        return;
    }
    strlcpy(g_cap_unitv.vision_api_key, config->api_key ? config->api_key : "",
            sizeof(g_cap_unitv.vision_api_key));
    strlcpy(g_cap_unitv.vision_model, config->model ? config->model : "",
            sizeof(g_cap_unitv.vision_model));
    strlcpy(g_cap_unitv.vision_base_url, config->base_url ? config->base_url : "",
            sizeof(g_cap_unitv.vision_base_url));
    strlcpy(g_cap_unitv.vision_backend_type,
            (config->backend_type && config->backend_type[0]) ? config->backend_type : "openai_compatible",
            sizeof(g_cap_unitv.vision_backend_type));
    strlcpy(g_cap_unitv.vision_auth_type,
            (config->auth_type && config->auth_type[0]) ? config->auth_type : "bearer",
            sizeof(g_cap_unitv.vision_auth_type));
    g_cap_unitv.vision_cfg = *config;
    g_cap_unitv.vision_cfg.api_key = g_cap_unitv.vision_api_key;
    g_cap_unitv.vision_cfg.model = g_cap_unitv.vision_model;
    g_cap_unitv.vision_cfg.base_url = g_cap_unitv.vision_base_url;
    g_cap_unitv.vision_cfg.backend_type = g_cap_unitv.vision_backend_type;
    g_cap_unitv.vision_cfg.auth_type = g_cap_unitv.vision_auth_type;
    if (g_cap_unitv.vision_cfg.timeout_ms == 0) g_cap_unitv.vision_cfg.timeout_ms = 30000;
    if (g_cap_unitv.vision_cfg.max_response_tokens == 0) g_cap_unitv.vision_cfg.max_response_tokens = 256;
    g_cap_unitv.vision_configured = (g_cap_unitv.vision_api_key[0] && g_cap_unitv.vision_model[0]);
    ESP_LOGI(TAG, "Vision config set: model=%s configured=%d",
             g_cap_unitv.vision_model, (int)g_cap_unitv.vision_configured);
}

static esp_err_t read_until_newline(char *buf, size_t buf_size, TickType_t deadline)
{
    int pos = 0;
    while (pos < (int)buf_size - 1) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) return ESP_ERR_TIMEOUT;
        uint8_t b;
        int rd = uart_read_bytes(g_cap_unitv.cfg.uart_port, &b, 1, deadline - now);
        if (rd <= 0) return ESP_ERR_TIMEOUT;
        if (b == '\n') break;
        if (b >= 0x20) buf[pos++] = (char)b;
    }
    buf[pos] = '\0';
    return pos > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t cap_unitv_uart_cmd(const char *cmd, const char *args_json,
                             char *resp, size_t resp_size, int timeout_ms)
{
    if (!g_cap_unitv.initialized) return ESP_ERR_INVALID_STATE;
    if (!cmd || !resp || resp_size == 0) return ESP_ERR_INVALID_ARG;

    uint32_t rid = (uint32_t)atomic_fetch_add(&g_cap_unitv.req_seq, 1) + 1;
    char req[256];
    int n = snprintf(req, sizeof(req),
                     "{\"cmd\":\"%s\",\"req_id\":\"%" PRIu32 "\",\"args\":%s}\n",
                     cmd, rid, args_json ? args_json : "{}");
    if (n <= 0 || n >= (int)sizeof(req)) return ESP_ERR_NO_MEM;

    xSemaphoreTake(g_cap_unitv.uart_mutex, portMAX_DELAY);
    uart_flush_input(g_cap_unitv.cfg.uart_port);
    int sent = uart_write_bytes(g_cap_unitv.cfg.uart_port, req, n);
    if (sent != n) {
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_FAIL;
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    esp_err_t err = read_until_newline(resp, resp_size, deadline);
    xSemaphoreGive(g_cap_unitv.uart_mutex);

    if (err == ESP_OK) atomic_store(&g_cap_unitv.available, true);
    return err;
}

esp_err_t cap_unitv_uart_capture_jpeg(int quality, uint8_t **jpeg_out, size_t *jpeg_size_out)
{
    if (!g_cap_unitv.initialized) return ESP_ERR_INVALID_STATE;
    if (!jpeg_out || !jpeg_size_out) return ESP_ERR_INVALID_ARG;
    *jpeg_out = NULL;
    *jpeg_size_out = 0;

    uint32_t rid = (uint32_t)atomic_fetch_add(&g_cap_unitv.req_seq, 1) + 1;
    char req[128];
    int n = snprintf(req, sizeof(req),
                     "{\"cmd\":\"CAPTURE\",\"req_id\":\"%" PRIu32 "\",\"args\":{\"quality\":%d}}\n",
                     rid, quality);
    if (n <= 0 || n >= (int)sizeof(req)) return ESP_ERR_NO_MEM;

    xSemaphoreTake(g_cap_unitv.uart_mutex, portMAX_DELAY);
    uart_flush_input(g_cap_unitv.cfg.uart_port);
    int sent = uart_write_bytes(g_cap_unitv.cfg.uart_port, req, n);
    if (sent != n) {
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_FAIL;
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(g_cap_unitv.cfg.capture_timeout_ms);

    /* Phase 1: read JSON header line. */
    char hdr[256];
    esp_err_t err = read_until_newline(hdr, sizeof(hdr), deadline);
    if (err != ESP_OK) {
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return err;
    }
    cJSON *root = cJSON_Parse(hdr);
    if (!root) {
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        cJSON_Delete(root);
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_FAIL;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *size_field = result ? cJSON_GetObjectItem(result, "size") : NULL;
    if (!size_field || !cJSON_IsNumber(size_field)) {
        cJSON_Delete(root);
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }
    int jpeg_size = size_field->valueint;
    cJSON_Delete(root);
    if (jpeg_size <= 0 || jpeg_size > g_cap_unitv.cfg.max_jpeg_bytes) {
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Phase 2: read binary JPEG bytes. */
    uint8_t *buf = (uint8_t *)malloc((size_t)jpeg_size);
    if (!buf) {
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_ERR_NO_MEM;
    }
    int total = 0;
    while (total < jpeg_size) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) { free(buf); xSemaphoreGive(g_cap_unitv.uart_mutex); return ESP_ERR_TIMEOUT; }
        int want = jpeg_size - total;
        if (want > 2048) want = 2048;
        int rd = uart_read_bytes(g_cap_unitv.cfg.uart_port, buf + total, want, deadline - now);
        if (rd <= 0) { free(buf); xSemaphoreGive(g_cap_unitv.uart_mutex); return ESP_ERR_TIMEOUT; }
        total += rd;
    }
    xSemaphoreGive(g_cap_unitv.uart_mutex);
    atomic_store(&g_cap_unitv.available, true);
    *jpeg_out = buf;
    *jpeg_size_out = (size_t)jpeg_size;
    ESP_LOGI(TAG, "JPEG captured: %d bytes", jpeg_size);
    return ESP_OK;
}
```

- [ ] **Step 5: Commit**

```bash
git add components/claw_capabilities/cap_unitv/CMakeLists.txt \
        components/claw_capabilities/cap_unitv/include/cap_unitv.h \
        components/claw_capabilities/cap_unitv/src/cap_unitv_internal.h \
        components/claw_capabilities/cap_unitv/src/cap_unitv_uart.c
git commit -m "feat(cap_unitv): add UART transport for SCAN and CAPTURE commands"
```

---

## Task 5: cap_unitv — Vision LLM call

**Files:**
- Create: `components/claw_capabilities/cap_unitv/src/cap_unitv_capture.c`

- [ ] **Step 1: Create `src/cap_unitv_capture.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_unitv_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

static const char *TAG = "cap_unitv_capture";

#define UNITV_VISION_BODY_INITIAL  (8 * 1024)
#define UNITV_VISION_RESP_INITIAL  (4 * 1024)

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} dynbuf_t;

static esp_err_t dynbuf_append(dynbuf_t *db, const char *data, size_t n)
{
    if (db->len + n + 1 > db->cap) {
        size_t new_cap = db->cap ? db->cap * 2 : 1024;
        while (new_cap < db->len + n + 1) new_cap *= 2;
        char *p = (char *)realloc(db->buf, new_cap);
        if (!p) return ESP_ERR_NO_MEM;
        db->buf = p;
        db->cap = new_cap;
    }
    memcpy(db->buf + db->len, data, n);
    db->len += n;
    db->buf[db->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    dynbuf_t *db = (dynbuf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && db) {
        return dynbuf_append(db, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

static esp_err_t base64_encode_jpeg(const uint8_t *jpeg, size_t jpeg_size, char **out_b64)
{
    *out_b64 = NULL;
    size_t enc_len = 0;
    /* Probe required size. */
    int rc = mbedtls_base64_encode(NULL, 0, &enc_len, jpeg, jpeg_size);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) return ESP_FAIL;
    char *buf = (char *)malloc(enc_len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    rc = mbedtls_base64_encode((unsigned char *)buf, enc_len, &enc_len, jpeg, jpeg_size);
    if (rc != 0) { free(buf); return ESP_FAIL; }
    buf[enc_len] = '\0';
    *out_b64 = buf;
    return ESP_OK;
}

static esp_err_t build_openai_request_body(const char *model, const char *question,
                                           const char *image_b64, uint32_t max_tokens,
                                           dynbuf_t *body)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddNumberToObject(root, "max_tokens", (double)max_tokens);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON *content = cJSON_AddArrayToObject(user_msg, "content");

    cJSON *image_part = cJSON_CreateObject();
    cJSON_AddStringToObject(image_part, "type", "image_url");
    cJSON *image_url = cJSON_AddObjectToObject(image_part, "image_url");
    /* "url" is a data URI. We need the prefix + base64. */
    size_t url_len = strlen("data:image/jpeg;base64,") + strlen(image_b64) + 1;
    char *url = (char *)malloc(url_len);
    if (!url) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
    snprintf(url, url_len, "data:image/jpeg;base64,%s", image_b64);
    cJSON_AddStringToObject(image_url, "url", url);
    free(url);
    cJSON_AddItemToArray(content, image_part);

    cJSON *text_part = cJSON_CreateObject();
    cJSON_AddStringToObject(text_part, "type", "text");
    cJSON_AddStringToObject(text_part, "text", question);
    cJSON_AddItemToArray(content, text_part);

    cJSON_AddItemToArray(messages, user_msg);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = dynbuf_append(body, json, strlen(json));
    free(json);
    return err;
}

static esp_err_t build_anthropic_request_body(const char *model, const char *question,
                                              const char *image_b64, uint32_t max_tokens,
                                              dynbuf_t *body)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddNumberToObject(root, "max_tokens", (double)max_tokens);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON *content = cJSON_AddArrayToObject(user_msg, "content");

    cJSON *image_part = cJSON_CreateObject();
    cJSON_AddStringToObject(image_part, "type", "image");
    cJSON *source = cJSON_AddObjectToObject(image_part, "source");
    cJSON_AddStringToObject(source, "type", "base64");
    cJSON_AddStringToObject(source, "media_type", "image/jpeg");
    cJSON_AddStringToObject(source, "data", image_b64);
    cJSON_AddItemToArray(content, image_part);

    cJSON *text_part = cJSON_CreateObject();
    cJSON_AddStringToObject(text_part, "type", "text");
    cJSON_AddStringToObject(text_part, "text", question);
    cJSON_AddItemToArray(content, text_part);

    cJSON_AddItemToArray(messages, user_msg);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = dynbuf_append(body, json, strlen(json));
    free(json);
    return err;
}

static esp_err_t extract_openai_text(const char *resp_json, char *out, size_t out_size)
{
    cJSON *root = cJSON_Parse(resp_json);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *first = cJSON_GetArrayItem(choices, 0);
        cJSON *message = first ? cJSON_GetObjectItem(first, "message") : NULL;
        cJSON *content = message ? cJSON_GetObjectItem(message, "content") : NULL;
        if (cJSON_IsString(content) && content->valuestring) {
            strlcpy(out, content->valuestring, out_size);
            err = ESP_OK;
        }
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t extract_anthropic_text(const char *resp_json, char *out, size_t out_size)
{
    cJSON *root = cJSON_Parse(resp_json);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (cJSON_IsArray(content)) {
        for (int i = 0; i < cJSON_GetArraySize(content); i++) {
            cJSON *block = cJSON_GetArrayItem(content, i);
            cJSON *type = block ? cJSON_GetObjectItem(block, "type") : NULL;
            cJSON *text = block ? cJSON_GetObjectItem(block, "text") : NULL;
            if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 &&
                cJSON_IsString(text) && text->valuestring) {
                strlcpy(out, text->valuestring, out_size);
                err = ESP_OK;
                break;
            }
        }
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t cap_unitv_vision_call(const char *question, const uint8_t *jpeg, size_t jpeg_size,
                                char *resp, size_t resp_size)
{
    if (!resp || resp_size == 0) return ESP_ERR_INVALID_ARG;
    if (!g_cap_unitv.vision_configured) return ESP_ERR_INVALID_STATE;
    if (!jpeg || jpeg_size == 0) return ESP_ERR_INVALID_ARG;

    bool is_anthropic = (strcmp(g_cap_unitv.vision_backend_type, "anthropic") == 0);

    char *image_b64 = NULL;
    esp_err_t err = base64_encode_jpeg(jpeg, jpeg_size, &image_b64);
    if (err != ESP_OK) return err;

    dynbuf_t body = {0};
    if (is_anthropic) {
        err = build_anthropic_request_body(g_cap_unitv.vision_model,
                                           question, image_b64,
                                           g_cap_unitv.vision_cfg.max_response_tokens, &body);
    } else {
        err = build_openai_request_body(g_cap_unitv.vision_model,
                                        question, image_b64,
                                        g_cap_unitv.vision_cfg.max_response_tokens, &body);
    }
    free(image_b64);
    if (err != ESP_OK) {
        free(body.buf);
        return err;
    }

    /* Build URL: base_url + "/chat/completions" (OpenAI) or "/messages" (Anthropic). */
    char url[256];
    const char *base = g_cap_unitv.vision_base_url[0] ? g_cap_unitv.vision_base_url
                                                      : (is_anthropic ? "https://api.anthropic.com/v1"
                                                                      : "https://api.openai.com/v1");
    const char *suffix = is_anthropic ? "/messages" : "/chat/completions";
    snprintf(url, sizeof(url), "%s%s", base, suffix);

    dynbuf_t resp_buf = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)g_cap_unitv.vision_cfg.timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &resp_buf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body.buf);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (is_anthropic) {
        esp_http_client_set_header(client, "x-api-key", g_cap_unitv.vision_api_key);
        esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
    } else {
        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", g_cap_unitv.vision_api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, body.buf, body.len);

    esp_err_t http_err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body.buf);

    if (http_err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Vision HTTP failed: err=%s status=%d", esp_err_to_name(http_err), status);
        free(resp_buf.buf);
        return http_err != ESP_OK ? http_err : ESP_FAIL;
    }

    if (is_anthropic) {
        err = extract_anthropic_text(resp_buf.buf, resp, resp_size);
    } else {
        err = extract_openai_text(resp_buf.buf, resp, resp_size);
    }
    free(resp_buf.buf);
    return err;
}
```

- [ ] **Step 2: Commit**

```bash
git add components/claw_capabilities/cap_unitv/src/cap_unitv_capture.c
git commit -m "feat(cap_unitv): add base64 + vision LLM HTTP call (OpenAI/Anthropic)"
```

---

## Task 6: cap_unitv — Capability registration, CLI, skills

**Files:**
- Create: `components/claw_capabilities/cap_unitv/src/cap_unitv.c`
- Create: `components/claw_capabilities/cap_unitv/src/cmd_cap_unitv.c`
- Create: `components/claw_capabilities/cap_unitv/skills/cap_unitv.md`
- Create: `components/claw_capabilities/cap_unitv/skills/skills_list.json`

- [ ] **Step 1: Create `src/cap_unitv.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_unitv_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"

static const char *TAG = "cap_unitv";

#define UNITV_SCAN_RESP_MAX     768
#define UNITV_CAPTURE_ANALYSIS_MAX  1024
#define UNITV_CAPTURE_DEFAULT_QUALITY 75

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static esp_err_t cap_unitv_scan_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)ctx;
    if (!output || output_size == 0) return ESP_ERR_INVALID_ARG;

    char mode[16] = "RELIABLE";
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    if (args) {
        cJSON *m = cJSON_GetObjectItem(args, "mode");
        if (m && cJSON_IsString(m) && m->valuestring &&
            (strcmp(m->valuestring, "fast") == 0 || strcmp(m->valuestring, "FAST") == 0)) {
            strlcpy(mode, "FAST", sizeof(mode));
        }
        cJSON_Delete(args);
    }
    char cmd_args[64];
    snprintf(cmd_args, sizeof(cmd_args), "{\"mode\":\"%s\",\"frames\":1}", mode);

    char raw[UNITV_SCAN_RESP_MAX];
    esp_err_t err = cap_unitv_uart_cmd("SCAN", cmd_args, raw, sizeof(raw),
                                       g_cap_unitv.cfg.default_timeout_ms);
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "{\"status\":\"camera_unavailable\",\"action\":\"unitv_scan\"}");
        return ESP_OK;
    }
    /* Pass through the raw JSON if it parses, otherwise wrap. */
    cJSON *root = cJSON_Parse(raw);
    if (!root) {
        snprintf(output, output_size,
                 "{\"status\":\"invalid_response\",\"action\":\"unitv_scan\"}");
        return ESP_OK;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (result) {
        char *result_str = cJSON_PrintUnformatted(result);
        if (result_str) {
            snprintf(output, output_size,
                     "{\"status\":\"ok\",\"action\":\"unitv_scan\",\"result\":%s}", result_str);
            free(result_str);
        } else {
            snprintf(output, output_size,
                     "{\"status\":\"memory_error\",\"action\":\"unitv_scan\"}");
        }
    } else {
        snprintf(output, output_size,
                 "{\"status\":\"ok\",\"action\":\"unitv_scan\",\"result\":{}}");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_unitv_capture_execute(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output, size_t output_size)
{
    (void)ctx;
    if (!output || output_size == 0) return ESP_ERR_INVALID_ARG;

    int quality = UNITV_CAPTURE_DEFAULT_QUALITY;
    char question[240] =
        "Describe what is visible in this rover camera image. Be concise and concrete.";
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    if (args) {
        cJSON *q = cJSON_GetObjectItem(args, "quality");
        if (q && cJSON_IsNumber(q)) quality = q->valueint;
        cJSON *prompt = cJSON_GetObjectItem(args, "question");
        if (prompt && cJSON_IsString(prompt) && prompt->valuestring && prompt->valuestring[0]) {
            strlcpy(question, prompt->valuestring, sizeof(question));
        }
        cJSON_Delete(args);
    }
    quality = clamp_int(quality, 30, 95);

    if (!g_cap_unitv.vision_configured) {
        snprintf(output, output_size,
                 "{\"status\":\"vision_not_configured\",\"action\":\"unitv_capture\"}");
        return ESP_OK;
    }

    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    esp_err_t err = cap_unitv_uart_capture_jpeg(quality, &jpeg, &jpeg_size);
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "{\"status\":\"camera_capture_failed\",\"action\":\"unitv_capture\"}");
        return ESP_OK;
    }

    char analysis[UNITV_CAPTURE_ANALYSIS_MAX];
    analysis[0] = '\0';
    err = cap_unitv_vision_call(question, jpeg, jpeg_size, analysis, sizeof(analysis));
    free(jpeg);

    if (err == ESP_ERR_INVALID_STATE) {
        snprintf(output, output_size,
                 "{\"status\":\"vision_not_configured\",\"action\":\"unitv_capture\"}");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "{\"status\":\"vision_failed\",\"action\":\"unitv_capture\","
                 "\"jpeg_bytes\":%u}", (unsigned)jpeg_size);
        return ESP_OK;
    }

    /* JSON-escape the analysis text. */
    cJSON *out = cJSON_CreateObject();
    if (!out) {
        snprintf(output, output_size,
                 "{\"status\":\"memory_error\",\"action\":\"unitv_capture\"}");
        return ESP_OK;
    }
    cJSON_AddStringToObject(out, "status", "ok");
    cJSON_AddStringToObject(out, "action", "unitv_capture");
    cJSON_AddNumberToObject(out, "jpeg_bytes", (double)jpeg_size);
    cJSON_AddStringToObject(out, "analysis", analysis);
    char *json_str = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (json_str) {
        strlcpy(output, json_str, output_size);
        free(json_str);
    } else {
        snprintf(output, output_size,
                 "{\"status\":\"memory_error\",\"action\":\"unitv_capture\"}");
    }
    return ESP_OK;
}

static const claw_cap_descriptor_t s_unitv_descriptors[] = {
    {
        .id = "unitv_scan",
        .name = "unitv_scan",
        .family = "vision",
        .description = "Run the camera's onboard scan (faces and objects). Fast structured output.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
              "\"mode\":{\"type\":\"string\",\"enum\":[\"fast\",\"reliable\"]}"
            "}}",
        .execute = cap_unitv_scan_execute,
    },
    {
        .id = "unitv_capture",
        .name = "unitv_capture",
        .family = "vision",
        .description = "Capture a JPEG and ask a vision LLM to analyze it. Use for detailed scene questions.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
              "\"question\":{\"type\":\"string\",\"description\":\"What to inspect in the image\"},"
              "\"quality\":{\"type\":\"integer\",\"minimum\":30,\"maximum\":95}"
            "}}",
        .execute = cap_unitv_capture_execute,
    },
};

static const claw_cap_group_t s_unitv_group = {
    .group_id = "cap_unitv",
    .plugin_name = "cap_unitv",
    .descriptors = s_unitv_descriptors,
    .descriptor_count = sizeof(s_unitv_descriptors) / sizeof(s_unitv_descriptors[0]),
};

esp_err_t cap_unitv_register_group(void)
{
    if (claw_cap_group_exists(s_unitv_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_unitv_group);
}
```

- [ ] **Step 2: Create `src/cmd_cap_unitv.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"
#include "cap_unitv.h"
#include "cap_unitv_internal.h"

static const char *TAG = "cmd_cap_unitv";

static int cmd_unitv_scan(int argc, char **argv)
{
    (void)argc; (void)argv;
    char resp[1024];
    esp_err_t err = cap_unitv_uart_cmd("SCAN", "{\"mode\":\"FAST\",\"frames\":1}",
                                       resp, sizeof(resp), 5000);
    if (err != ESP_OK) {
        printf("scan: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("scan: %s\n", resp);
    return 0;
}

static int cmd_unitv_capture(int argc, char **argv)
{
    int quality = (argc >= 2) ? atoi(argv[1]) : 75;
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    esp_err_t err = cap_unitv_uart_capture_jpeg(quality, &jpeg, &jpeg_size);
    if (err != ESP_OK) {
        printf("capture: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("capture: %u bytes\n", (unsigned)jpeg_size);
    free(jpeg);
    return 0;
}

void cap_unitv_register_cli(void)
{
    const esp_console_cmd_t scan_cmd = {
        .command = "unitv_scan",
        .help = "UnitV SCAN command (fast)",
        .func = cmd_unitv_scan,
    };
    const esp_console_cmd_t capture_cmd = {
        .command = "unitv_capture",
        .help = "UnitV CAPTURE; arg: quality 30-95",
        .func = cmd_unitv_capture,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&scan_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&capture_cmd));
    ESP_LOGI(TAG, "cap_unitv CLI commands registered");
}
```

- [ ] **Step 3: Create `skills/cap_unitv.md`**

```markdown
# UnitV Camera

You have a fixed front-facing camera (UnitV-M12). It cannot pan or tilt —
to look elsewhere, drive the rover with `rover_turn` or `rover_move`.

## Tools

### unitv_scan(mode)

Fast onboard scan. Returns structured JSON describing detected faces and
recognizable objects. Use for quick "is anyone there?" or "is the room
empty?" type questions.
- `mode`: "fast" (lower frame count) or "reliable" (default).

### unitv_capture(question, quality)

Capture a JPEG and ask a vision LLM to analyze it. Use for detailed scene
analysis: colors, named objects (sofa, chair, mug), text in the frame,
spatial layout.
- `question`: phrase the user's question for the vision model. Be specific
  (e.g., "Is there a red mug on the desk?", not just "what do you see?").
- `quality`: JPEG quality 30..95, default 75.

Returns `{"status":"ok","jpeg_bytes":N,"analysis":"<text>"}` or an error
status: `camera_unavailable`, `camera_capture_failed`, `vision_not_configured`,
`vision_failed`.

## Conventions

- Prefer `unitv_scan` for yes/no presence questions — it's faster and uses
  no LLM tokens.
- For detailed visual questions, use `unitv_capture` with a focused question.
- After moving the rover, allow ~500ms before the next capture (motion blur).
- Do not loop scan/capture more than 4 times in a single user turn — if the
  scene is unclear after 4 attempts, ask the user.
```

- [ ] **Step 4: Create `skills/skills_list.json`**

```json
{
  "skills": [
    {
      "id": "cap_unitv",
      "file": "cap_unitv.md",
      "summary": "Use the rover's camera: structured scan or vision-LLM capture.",
      "cap_groups": ["cap_unitv"]
    }
  ]
}
```

- [ ] **Step 5: Commit**

```bash
git add components/claw_capabilities/cap_unitv/src/cap_unitv.c \
        components/claw_capabilities/cap_unitv/src/cmd_cap_unitv.c \
        components/claw_capabilities/cap_unitv/skills/
git commit -m "feat(cap_unitv): add capability descriptors, CLI, and skill document"
```

---

## Task 7: rover_demo — application scaffold

**Files:**
- Create: `application/rover_demo/CMakeLists.txt`
- Create: `application/rover_demo/sdkconfig.defaults`
- Create: `application/rover_demo/partitions.csv`
- Create: `application/rover_demo/idf_component.yml`
- Create: `application/rover_demo/main/CMakeLists.txt`
- Create: `application/rover_demo/main/idf_component.yml`
- Create: `application/rover_demo/main/Kconfig.projbuild`

- [ ] **Step 1: Create `application/rover_demo/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rover_demo)

# Bake fatfs_image into a "storage" partition.
fatfs_create_spiflash_image(storage fatfs_image FLASH_IN_PROJECT)
```

- [ ] **Step 2: Create `application/rover_demo/sdkconfig.defaults`**

```
# CPU
CONFIG_ESP32_DEFAULT_CPU_FREQ_240=y

# No PSRAM on M5StickC Plus
CONFIG_SPIRAM_SUPPORT=n

# Stack sizes — leaner than basic_demo
CONFIG_ESP_MAIN_TASK_STACK_SIZE=6144
CONFIG_ESP_TIMER_TASK_STACK_SIZE=4096

# TLS heap optimization
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y

# HTTP client — needed for vision LLM call
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y

# Logging
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y

# Console
CONFIG_ESP_CONSOLE_UART_DEFAULT=y

# FAT
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_MAX_LFN=255
CONFIG_FATFS_API_ENCODING_UTF_8=y

# Watchdog
CONFIG_ESP_TASK_WDT_INIT=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=15

# Partition table
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# Flash size for M5StickC Plus
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
```

- [ ] **Step 3: Create `application/rover_demo/partitions.csv`**

```
# Name,    Type, SubType, Offset,   Size,     Flags
nvs,       data, nvs,     0x9000,   0x4000
otadata,   data, ota,     0xd000,   0x2000
phy_init,  data, phy,     0xf000,   0x1000
factory,   app,  factory, 0x10000,  0x180000
storage,   data, fat,     0x190000, 0x270000
```

(1.5MB factory app + ~2.4MB FATFS in 4MB flash, leaving room for partition tables and bootloader.)

- [ ] **Step 4: Create `application/rover_demo/idf_component.yml`**

```yaml
dependencies:
  idf: ">=5.3.0"
  m5stack/m5unified:
    version: "^0.2.0"
  espressif/mdns: "^1.2.0"
```

- [ ] **Step 5: Create `application/rover_demo/main/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS
        "main.c"
        "rover_demo_settings.c"
        "rover_demo_wifi.c"
        "rover_buttons.c"
        "rover_display.cpp"
        "app_rover.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        fatfs
        wear_levelling
        console
        driver
        esp_timer
        m5stack__m5unified
        cap_rover
        cap_unitv
        cap_im_tg
        cap_files
        cap_skill_mgr
        cap_session_mgr
        cap_system
        cap_time
        claw_core
        claw_cap
        claw_event_router
        claw_memory
        claw_skill
)

# Pull in board-specific setup_device source. Board name comes from sdkconfig.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_SOURCE_DIR}/boards/m5stickc_plus/setup_device.cpp"
)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_SOURCE_DIR}/boards/m5stickc_plus"
)
```

- [ ] **Step 6: Create `application/rover_demo/main/idf_component.yml`**

```yaml
dependencies:
  m5stack/m5unified:
    version: "^0.2.0"
```

- [ ] **Step 7: Create `application/rover_demo/main/Kconfig.projbuild`**

```
menu "Rover Demo Configuration"

config ROVER_DEMO_WIFI_SSID
    string "Default WiFi SSID"
    default ""

config ROVER_DEMO_WIFI_PASSWORD
    string "Default WiFi password"
    default ""

config ROVER_DEMO_LLM_API_KEY
    string "Default LLM API key"
    default ""

config ROVER_DEMO_LLM_BACKEND_TYPE
    string "Default LLM backend type"
    default "openai_compatible"
    help
        Either "openai_compatible" or "anthropic".

config ROVER_DEMO_LLM_PROFILE
    string "Default LLM profile"
    default "openai"

config ROVER_DEMO_LLM_MODEL
    string "Default LLM model"
    default "openrouter/auto"

config ROVER_DEMO_LLM_BASE_URL
    string "Default LLM base URL"
    default "https://openrouter.ai/api/v1"

config ROVER_DEMO_LLM_AUTH_TYPE
    string "Default LLM auth type"
    default "bearer"

config ROVER_DEMO_LLM_TIMEOUT_MS
    string "Default LLM timeout (ms)"
    default "30000"

config ROVER_DEMO_TG_BOT_TOKEN
    string "Default Telegram bot token"
    default ""

config ROVER_DEMO_TIME_TIMEZONE
    string "Default timezone (POSIX TZ string)"
    default "UTC0"

endmenu
```

- [ ] **Step 8: Commit**

```bash
git add application/rover_demo/CMakeLists.txt \
        application/rover_demo/sdkconfig.defaults \
        application/rover_demo/partitions.csv \
        application/rover_demo/idf_component.yml \
        application/rover_demo/main/CMakeLists.txt \
        application/rover_demo/main/idf_component.yml \
        application/rover_demo/main/Kconfig.projbuild
git commit -m "feat(rover_demo): scaffold application directory and Kconfig"
```

---

## Task 8: M5StickC Plus board support + setup_device.cpp

**Files:**
- Create: `application/rover_demo/boards/m5stickc_plus/board_info.yaml`
- Create: `application/rover_demo/boards/m5stickc_plus/board_peripherals.yaml`
- Create: `application/rover_demo/boards/m5stickc_plus/board_devices.yaml`
- Create: `application/rover_demo/boards/m5stickc_plus/sdkconfig.defaults.board`
- Create: `application/rover_demo/boards/m5stickc_plus/setup_device.h`
- Create: `application/rover_demo/boards/m5stickc_plus/setup_device.cpp`

- [ ] **Step 1: Create `boards/m5stickc_plus/board_info.yaml`**

```yaml
board:
  name: m5stickc_plus
  display_name: "M5StickC Plus"
  chip: esp32
  flash: "4MB"
  description: |
    M5StickC Plus controller (ESP32-PICO-D4) with the RoverC Pro mecanum
    base attached via the HAT connector and the UnitV-M12 camera attached
    via the Grove (HY2.0) connector.
```

- [ ] **Step 2: Create `boards/m5stickc_plus/board_peripherals.yaml`**

```yaml
peripherals:
  - name: rover_i2c
    type: i2c
    port: 1
    sda: 0
    scl: 26
    frequency: 100000
  - name: vision_uart
    type: uart
    port: 1
    tx: 32
    rx: 33
    baud: 115200
  - name: btn_a
    type: gpio
    pin: 37
    direction: input
  - name: btn_b
    type: gpio
    pin: 39
    direction: input
```

- [ ] **Step 3: Create `boards/m5stickc_plus/board_devices.yaml`**

```yaml
devices:
  - name: roverc_pro
    type: i2c_device
    bus: rover_i2c
    address: 0x38
    description: "K036-B RoverC Pro mecanum base"
  - name: unitv_m12
    type: uart_device
    bus: vision_uart
    description: "UnitV-M12 K210 AI camera"
```

- [ ] **Step 4: Create `boards/m5stickc_plus/sdkconfig.defaults.board`**

```
CONFIG_IDF_TARGET="esp32"
CONFIG_IDF_TARGET_ESP32=y
CONFIG_FREERTOS_UNICORE=n
CONFIG_FREERTOS_HZ=1000
```

- [ ] **Step 5: Create `boards/m5stickc_plus/setup_device.h`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize M5Unified: display, IMU, power management, internal I2C. */
esp_err_t rover_board_init(void);

/* Pump M5Unified state machines (button debounce, etc). Call every ~20ms. */
void rover_board_m5_update(void);

/* Buttons (after rover_board_m5_update). */
bool rover_board_btn_a_pressed(void);
bool rover_board_btn_b_pressed(void);
bool rover_board_btn_a_was_pressed(void);
bool rover_board_btn_b_was_pressed(void);

/* Battery 0..100; -1 if unavailable. */
int  rover_board_get_battery_pct(void);

/* IMU. Returns ESP_ERR_NOT_SUPPORTED if not enabled. */
bool     rover_board_imu_enabled(void);
esp_err_t rover_board_imu_read(float *ax, float *ay, float *az,
                               float *gx, float *gy, float *gz);

/* Display. */
typedef enum {
    ROVER_DISPLAY_STATE_BOOT = 0,
    ROVER_DISPLAY_STATE_IDLE,
    ROVER_DISPLAY_STATE_THINKING,
    ROVER_DISPLAY_STATE_EXECUTING,
    ROVER_DISPLAY_STATE_OFFLINE,
    ROVER_DISPLAY_STATE_SLEEPING,
} rover_display_state_t;

void rover_board_display_render(rover_display_state_t state,
                                const char *ip,
                                int battery_pct);
void rover_board_display_sleep(void);
void rover_board_display_set_brightness(uint8_t brightness);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 6: Create `boards/m5stickc_plus/setup_device.cpp`**

```cpp
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include "M5Unified.h"

extern "C" {
#include "setup_device.h"
}

static bool s_imu_ok = false;

extern "C" esp_err_t rover_board_init(void)
{
    auto cfg = M5.config();
    cfg.output_power = true;
    cfg.internal_imu = true;
    cfg.internal_rtc = true;
    cfg.internal_mic = false;
    cfg.internal_spk = false;
    cfg.led_brightness = 0;

    M5.begin(cfg);
    M5.Display.setRotation(3);            /* landscape 240x135 */
    M5.Display.setBrightness(128);
    M5.Display.fillScreen(TFT_BLACK);
    s_imu_ok = M5.Imu.isEnabled();
    return ESP_OK;
}

extern "C" void rover_board_m5_update(void)
{
    M5.update();
}

extern "C" bool rover_board_btn_a_pressed(void)        { return M5.BtnA.isPressed(); }
extern "C" bool rover_board_btn_b_pressed(void)        { return M5.BtnB.isPressed(); }
extern "C" bool rover_board_btn_a_was_pressed(void)    { return M5.BtnA.wasPressed(); }
extern "C" bool rover_board_btn_b_was_pressed(void)    { return M5.BtnB.wasPressed(); }

extern "C" int rover_board_get_battery_pct(void)
{
    int pct = M5.Power.getBatteryLevel();
    if (pct < 0) return -1;
    if (pct > 100) pct = 100;
    return pct;
}

extern "C" bool rover_board_imu_enabled(void)
{
    return s_imu_ok;
}

extern "C" esp_err_t rover_board_imu_read(float *ax, float *ay, float *az,
                                          float *gx, float *gy, float *gz)
{
    if (!s_imu_ok) return ESP_ERR_NOT_SUPPORTED;
    bool a_ok = M5.Imu.getAccel(ax, ay, az);
    bool g_ok = M5.Imu.getGyro(gx, gy, gz);
    return (a_ok && g_ok) ? ESP_OK : ESP_FAIL;
}

static rover_display_state_t s_last_state = (rover_display_state_t)-1;
static char s_last_ip[20] = {0};
static int s_last_batt = -2;

static const char *state_label(rover_display_state_t s)
{
    switch (s) {
    case ROVER_DISPLAY_STATE_BOOT:      return "BOOT";
    case ROVER_DISPLAY_STATE_IDLE:      return "IDLE";
    case ROVER_DISPLAY_STATE_THINKING:  return "AI_THINK";
    case ROVER_DISPLAY_STATE_EXECUTING: return "AI_EXEC";
    case ROVER_DISPLAY_STATE_OFFLINE:   return "OFFLINE";
    case ROVER_DISPLAY_STATE_SLEEPING:  return "SLEEP";
    default:                            return "?";
    }
}

static uint32_t state_color(rover_display_state_t s)
{
    switch (s) {
    case ROVER_DISPLAY_STATE_IDLE:      return 0x2563EBu; /* blue */
    case ROVER_DISPLAY_STATE_THINKING:
    case ROVER_DISPLAY_STATE_EXECUTING: return 0xEA580Cu; /* orange */
    case ROVER_DISPLAY_STATE_OFFLINE:   return 0xDC2626u; /* red */
    case ROVER_DISPLAY_STATE_SLEEPING:  return 0x6B21A8u; /* purple */
    default:                            return 0x111827u; /* gray */
    }
}

extern "C" void rover_board_display_render(rover_display_state_t state,
                                           const char *ip,
                                           int battery_pct)
{
    /* Skip redraw if nothing visible has changed. */
    const char *ip_str = ip ? ip : "";
    if (state == s_last_state &&
        strcmp(ip_str, s_last_ip) == 0 &&
        battery_pct == s_last_batt) {
        return;
    }
    s_last_state = state;
    strlcpy(s_last_ip, ip_str, sizeof(s_last_ip));
    s_last_batt = battery_pct;

    const uint32_t bg = 0x111827u;
    const uint32_t bar = state_color(state);

    M5.Display.startWrite();
    M5.Display.fillScreen(bg);

    /* Top bar */
    M5.Display.fillRoundRect(2, 2, 236, 24, 4, bar);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_WHITE, bar);
    M5.Display.setCursor(8, 6);
    M5.Display.print("AI Rover");

    /* Centered state label */
    const char *label = state_label(state);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(TFT_WHITE, bg);
    int label_w = (int)strlen(label) * 18;
    M5.Display.setCursor((240 - label_w) / 2, 50);
    M5.Display.print(label);

    /* Bottom: IP + battery */
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x9CA3AFu, bg);
    M5.Display.setCursor(8, 116);
    if (ip_str[0]) {
        M5.Display.print(ip_str);
    } else {
        M5.Display.print("no wifi");
    }
    if (battery_pct >= 0) {
        char batt_buf[8];
        snprintf(batt_buf, sizeof(batt_buf), "%d%%", battery_pct);
        int batt_w = (int)strlen(batt_buf) * 6;
        M5.Display.setCursor(240 - batt_w - 8, 116);
        M5.Display.print(batt_buf);
    }
    M5.Display.endWrite();
}

extern "C" void rover_board_display_sleep(void)
{
    M5.Display.setBrightness(0);
    M5.Display.sleep();
}

extern "C" void rover_board_display_set_brightness(uint8_t brightness)
{
    M5.Display.setBrightness(brightness);
}
```

- [ ] **Step 7: Commit**

```bash
git add application/rover_demo/boards/
git commit -m "feat(rover_demo): add M5StickC Plus board support and setup_device wrapper"
```

---

## Task 9: Settings and WiFi

**Files:**
- Create: `application/rover_demo/main/rover_demo_settings.h`
- Create: `application/rover_demo/main/rover_demo_settings.c`
- Create: `application/rover_demo/main/rover_demo_wifi.h`
- Create: `application/rover_demo/main/rover_demo_wifi.c`

- [ ] **Step 1: Create `main/rover_demo_settings.h`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#define ROVER_DEMO_STR_LEN       320
#define ROVER_DEMO_TIMEZONE_LEN  32

typedef struct {
    char wifi_ssid[ROVER_DEMO_STR_LEN];
    char wifi_password[ROVER_DEMO_STR_LEN];
    char llm_api_key[ROVER_DEMO_STR_LEN];
    char llm_backend_type[32];
    char llm_profile[32];
    char llm_model[64];
    char llm_base_url[ROVER_DEMO_STR_LEN];
    char llm_auth_type[32];
    char llm_timeout_ms[16];
    char tg_bot_token[ROVER_DEMO_STR_LEN];
    char time_timezone[ROVER_DEMO_TIMEZONE_LEN];
} rover_demo_settings_t;

esp_err_t rover_demo_settings_init(void);
esp_err_t rover_demo_settings_load(rover_demo_settings_t *settings);
esp_err_t rover_demo_settings_save(const rover_demo_settings_t *settings);

extern const char *rover_demo_fatfs_base_path;
```

- [ ] **Step 2: Create `main/rover_demo_settings.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_demo_settings.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "rover_demo_settings";
static const char *NS = "rover_demo";

const char *rover_demo_fatfs_base_path = "/fatfs";

typedef struct {
    const char *key;
    const char *def;
    char *buf;
    size_t buf_size;
} field_t;

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strlcpy(dst, src, dst_size);
}

static void load_defaults(rover_demo_settings_t *s)
{
    copy_str(s->wifi_ssid,         sizeof(s->wifi_ssid),         CONFIG_ROVER_DEMO_WIFI_SSID);
    copy_str(s->wifi_password,     sizeof(s->wifi_password),     CONFIG_ROVER_DEMO_WIFI_PASSWORD);
    copy_str(s->llm_api_key,       sizeof(s->llm_api_key),       CONFIG_ROVER_DEMO_LLM_API_KEY);
    copy_str(s->llm_backend_type,  sizeof(s->llm_backend_type),  CONFIG_ROVER_DEMO_LLM_BACKEND_TYPE);
    copy_str(s->llm_profile,       sizeof(s->llm_profile),       CONFIG_ROVER_DEMO_LLM_PROFILE);
    copy_str(s->llm_model,         sizeof(s->llm_model),         CONFIG_ROVER_DEMO_LLM_MODEL);
    copy_str(s->llm_base_url,      sizeof(s->llm_base_url),      CONFIG_ROVER_DEMO_LLM_BASE_URL);
    copy_str(s->llm_auth_type,     sizeof(s->llm_auth_type),     CONFIG_ROVER_DEMO_LLM_AUTH_TYPE);
    copy_str(s->llm_timeout_ms,    sizeof(s->llm_timeout_ms),    CONFIG_ROVER_DEMO_LLM_TIMEOUT_MS);
    copy_str(s->tg_bot_token,      sizeof(s->tg_bot_token),      CONFIG_ROVER_DEMO_TG_BOT_TOKEN);
    copy_str(s->time_timezone,     sizeof(s->time_timezone),     CONFIG_ROVER_DEMO_TIME_TIMEZONE);
}

esp_err_t rover_demo_settings_init(void)
{
    return ESP_OK;
}

esp_err_t rover_demo_settings_load(rover_demo_settings_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    memset(s, 0, sizeof(*s));
    load_defaults(s);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s — using defaults", esp_err_to_name(err));
        return ESP_OK;
    }

    field_t fields[] = {
        {"wifi_ssid",        NULL, s->wifi_ssid,        sizeof(s->wifi_ssid)},
        {"wifi_password",    NULL, s->wifi_password,    sizeof(s->wifi_password)},
        {"llm_api_key",      NULL, s->llm_api_key,      sizeof(s->llm_api_key)},
        {"llm_backend",      NULL, s->llm_backend_type, sizeof(s->llm_backend_type)},
        {"llm_profile",      NULL, s->llm_profile,      sizeof(s->llm_profile)},
        {"llm_model",        NULL, s->llm_model,        sizeof(s->llm_model)},
        {"llm_base_url",     NULL, s->llm_base_url,     sizeof(s->llm_base_url)},
        {"llm_auth",         NULL, s->llm_auth_type,    sizeof(s->llm_auth_type)},
        {"llm_timeout_ms",   NULL, s->llm_timeout_ms,   sizeof(s->llm_timeout_ms)},
        {"tg_bot_token",     NULL, s->tg_bot_token,     sizeof(s->tg_bot_token)},
        {"time_timezone",    NULL, s->time_timezone,    sizeof(s->time_timezone)},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        size_t len = fields[i].buf_size;
        esp_err_t e = nvs_get_str(h, fields[i].key, fields[i].buf, &len);
        if (e == ESP_OK) continue;
        if (e == ESP_ERR_NVS_NOT_FOUND) continue;
        ESP_LOGW(TAG, "nvs_get_str %s failed: %s", fields[i].key, esp_err_to_name(e));
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t rover_demo_settings_save(const rover_demo_settings_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    typedef struct { const char *key; const char *val; } pair_t;
    pair_t pairs[] = {
        {"wifi_ssid",      s->wifi_ssid},
        {"wifi_password",  s->wifi_password},
        {"llm_api_key",    s->llm_api_key},
        {"llm_backend",    s->llm_backend_type},
        {"llm_profile",    s->llm_profile},
        {"llm_model",      s->llm_model},
        {"llm_base_url",   s->llm_base_url},
        {"llm_auth",       s->llm_auth_type},
        {"llm_timeout_ms", s->llm_timeout_ms},
        {"tg_bot_token",   s->tg_bot_token},
        {"time_timezone",  s->time_timezone},
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        err = nvs_set_str(h, pairs[i].key, pairs[i].val);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
```

- [ ] **Step 3: Create `main/rover_demo_wifi.h`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*rover_demo_wifi_state_cb)(bool connected, void *user_ctx);

esp_err_t rover_demo_wifi_init(void);
esp_err_t rover_demo_wifi_register_state_cb(rover_demo_wifi_state_cb cb, void *user_ctx);
esp_err_t rover_demo_wifi_start(const char *ssid, const char *password);
bool      rover_demo_wifi_is_connected(void);
const char *rover_demo_wifi_get_ip(void);
esp_err_t rover_demo_wifi_wait_connected(uint32_t timeout_ms);
```

- [ ] **Step 4: Create `main/rover_demo_wifi.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_demo_wifi.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "rover_demo_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     20

static EventGroupHandle_t s_event_group;
static int s_retry = 0;
static bool s_connected = false;
static char s_ip_str[20] = {0};
static rover_demo_wifi_state_cb s_state_cb = NULL;
static void *s_state_cb_ctx = NULL;
static esp_netif_t *s_netif = NULL;

static void notify_state(bool connected)
{
    s_connected = connected;
    if (s_state_cb) s_state_cb(connected, s_state_cb_ctx);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip_str[0] = '\0';
        notify_state(false);
        if (s_retry < WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "wifi disconnect, retry %d/%d", s_retry, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
        }
        return;
    }
    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_retry = 0;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "got IP: %s", s_ip_str);
        notify_state(true);
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t rover_demo_wifi_init(void)
{
    if (s_event_group) return ESP_OK;
    s_event_group = xEventGroupCreate();
    if (!s_event_group) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_FAIL;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &event_handler, NULL, NULL);
    return ESP_OK;
}

esp_err_t rover_demo_wifi_register_state_cb(rover_demo_wifi_state_cb cb, void *user_ctx)
{
    s_state_cb = cb;
    s_state_cb_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t rover_demo_wifi_start(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password ? password : "", sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = password && password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return ESP_OK;
}

bool rover_demo_wifi_is_connected(void)         { return s_connected; }
const char *rover_demo_wifi_get_ip(void)        { return s_ip_str; }

esp_err_t rover_demo_wifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_event_group) return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(
        s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    return ESP_ERR_TIMEOUT;
}
```

- [ ] **Step 5: Commit**

```bash
git add application/rover_demo/main/rover_demo_settings.h \
        application/rover_demo/main/rover_demo_settings.c \
        application/rover_demo/main/rover_demo_wifi.h \
        application/rover_demo/main/rover_demo_wifi.c
git commit -m "feat(rover_demo): add settings persistence and WiFi STA driver"
```

---

## Task 10: Display and buttons

**Files:**
- Create: `application/rover_demo/main/rover_display.h`
- Create: `application/rover_demo/main/rover_display.cpp`
- Create: `application/rover_demo/main/rover_buttons.h`
- Create: `application/rover_demo/main/rover_buttons.c`

- [ ] **Step 1: Create `main/rover_display.h`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "setup_device.h"      /* for rover_display_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Set the displayed state. Triggers a redraw on next refresh. */
void rover_display_set_state(rover_display_state_t state);

/* Periodic refresh — call from a low-priority task every ~500ms. */
void rover_display_refresh(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create `main/rover_display.cpp`**

```cpp
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_display.h"

#include <atomic>
#include <string.h>

extern "C" {
#include "rover_demo_wifi.h"
#include "setup_device.h"
}

static std::atomic<rover_display_state_t> s_state{ROVER_DISPLAY_STATE_BOOT};

extern "C" void rover_display_set_state(rover_display_state_t state)
{
    s_state.store(state, std::memory_order_relaxed);
}

extern "C" void rover_display_refresh(void)
{
    rover_display_state_t state = s_state.load(std::memory_order_relaxed);
    const char *ip = rover_demo_wifi_get_ip();
    int batt = rover_board_get_battery_pct();
    rover_board_display_render(state, ip, batt);
}
```

- [ ] **Step 3: Create `main/rover_buttons.h`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "claw_event.h"

typedef esp_err_t (*rover_buttons_event_cb)(const claw_event_t *event, void *user_ctx);
typedef void      (*rover_buttons_sleep_cb)(void *user_ctx);

typedef struct {
    rover_buttons_event_cb  on_btn_a_short;     /* short press: inject demo event */
    void                   *on_btn_a_short_ctx;
    rover_buttons_sleep_cb  on_btn_a_long;      /* long press (>3s): enter deep sleep */
    void                   *on_btn_a_long_ctx;
} rover_buttons_config_t;

esp_err_t rover_buttons_init(const rover_buttons_config_t *config);
```

- [ ] **Step 4: Create `main/rover_buttons.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_buttons.h"

#include <string.h>
#include "cap_rover.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rover_display.h"
#include "setup_device.h"

static const char *TAG = "rover_buttons";

#define BTN_TASK_PERIOD_MS  20
#define BTN_LONG_PRESS_MS   3000

static rover_buttons_config_t s_cfg;

static void inject_demo_event(void)
{
    if (!s_cfg.on_btn_a_short) return;
    claw_event_t ev = {0};
    strlcpy(ev.event_id, "btn_a_demo", sizeof(ev.event_id));
    strlcpy(ev.source_cap, "rover_buttons", sizeof(ev.source_cap));
    strlcpy(ev.event_type, "message", sizeof(ev.event_type));
    strlcpy(ev.source_channel, "btn_a", sizeof(ev.source_channel));
    strlcpy(ev.chat_id, "local", sizeof(ev.chat_id));
    strlcpy(ev.sender_id, "local", sizeof(ev.sender_id));
    strlcpy(ev.content_type, "text", sizeof(ev.content_type));
    /* text and payload_json must be heap-allocated and freed by the consumer
     * via claw_event_free, OR clones can be made. For the simple synchronous
     * callback path, we let the cb take ownership/copy. */
    ev.text = strdup("demo");
    s_cfg.on_btn_a_short(&ev, s_cfg.on_btn_a_short_ctx);
    free(ev.text);
}

static void buttons_task(void *arg)
{
    (void)arg;
    TickType_t btn_a_pressed_at = 0;
    bool btn_a_was_down = false;

    while (1) {
        rover_board_m5_update();

        /* BtnB → cap_rover emergency stop */
        if (rover_board_btn_b_pressed()) {
            cap_rover_emergency_stop_set();
        }

        /* BtnA: edge detection, long press → sleep, short press → demo event */
        bool down = rover_board_btn_a_pressed();
        TickType_t now = xTaskGetTickCount();
        if (down && !btn_a_was_down) {
            btn_a_pressed_at = now;
        }
        if (!down && btn_a_was_down) {
            TickType_t held = now - btn_a_pressed_at;
            if (held >= pdMS_TO_TICKS(BTN_LONG_PRESS_MS)) {
                ESP_LOGI(TAG, "BtnA long-press → sleep");
                if (s_cfg.on_btn_a_long) s_cfg.on_btn_a_long(s_cfg.on_btn_a_long_ctx);
            } else {
                ESP_LOGI(TAG, "BtnA short-press → demo");
                inject_demo_event();
            }
        }
        btn_a_was_down = down;

        rover_display_refresh();
        vTaskDelay(pdMS_TO_TICKS(BTN_TASK_PERIOD_MS));
    }
}

esp_err_t rover_buttons_init(const rover_buttons_config_t *config)
{
    if (config) s_cfg = *config;
    BaseType_t ok = xTaskCreate(buttons_task, "rover_btn", 4096, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
```

- [ ] **Step 5: Commit**

```bash
git add application/rover_demo/main/rover_display.h \
        application/rover_demo/main/rover_display.cpp \
        application/rover_demo/main/rover_buttons.h \
        application/rover_demo/main/rover_buttons.c
git commit -m "feat(rover_demo): add display refresh and button polling task"
```

---

## Task 11: app_rover.c — application wiring

**Files:**
- Create: `application/rover_demo/main/app_rover.h`
- Create: `application/rover_demo/main/app_rover.c`

- [ ] **Step 1: Create `main/app_rover.h`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "rover_demo_settings.h"

esp_err_t app_rover_start(const rover_demo_settings_t *settings);
```

- [ ] **Step 2: Create `main/app_rover.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_rover.h"

#include <stdlib.h>
#include <string.h>

#include "cap_files.h"
#include "cap_im_tg.h"
#include "cap_rover.h"
#include "cap_session_mgr.h"
#include "cap_skill_mgr.h"
#include "cap_system.h"
#include "cap_time.h"
#include "cap_unitv.h"
#include "claw_cap.h"
#include "claw_core.h"
#include "claw_event_publisher.h"
#include "claw_event_router.h"
#include "claw_memory.h"
#include "claw_skill.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rover_buttons.h"
#include "rover_demo_wifi.h"
#include "rover_display.h"
#include "setup_device.h"

static const char *TAG = "app_rover";

#define ROVER_SYSTEM_PROMPT \
    "You are AI Rover — a mecanum robot with a gripper and a fixed front-facing camera. " \
    "Answer briefly in the user's language. " \
    "Use tools directly when the user gives a command — do not ask permission first. " \
    "Use rover_move() for timed movement, rover_turn() for precise rotation (uses IMU). " \
    "Use unitv_scan() for quick object detection. " \
    "Use unitv_capture(question=...) for detailed scene analysis. " \
    "Camera is fixed — to change the view, call rover_turn() or rover_move(). " \
    "For multi-step tasks, activate rover_ops or rover_search skill first."

static const char *const ROVER_LLM_VISIBLE_GROUPS[] = {
    "cap_rover",
    "cap_unitv",
    "cap_files",
    "cap_skill",
    "cap_system",
};

typedef struct {
    char memory_session_root[64];
    char memory_root_dir[64];
    char skills_root_dir[64];
    char router_rules_path[96];
    char im_attachment_root[64];
} rover_paths_t;

static rover_paths_t s_paths;

static esp_err_t init_paths(rover_paths_t *p)
{
    const char *base = rover_demo_fatfs_base_path;
    if (!p || !base || base[0] != '/') return ESP_ERR_INVALID_STATE;
    snprintf(p->memory_session_root, sizeof(p->memory_session_root), "%s/sessions", base);
    snprintf(p->memory_root_dir,     sizeof(p->memory_root_dir),     "%s/memory",   base);
    snprintf(p->skills_root_dir,     sizeof(p->skills_root_dir),     "%s/skills",   base);
    snprintf(p->router_rules_path,   sizeof(p->router_rules_path),   "%s/router_rules/router_rules.json", base);
    snprintf(p->im_attachment_root,  sizeof(p->im_attachment_root),  "%s/inbox",    base);
    return ESP_OK;
}

static esp_err_t imu_read_adapter(float *ax, float *ay, float *az,
                                  float *gx, float *gy, float *gz)
{
    return rover_board_imu_read(ax, ay, az, gx, gy, gz);
}

static esp_err_t demo_event_cb(const claw_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (!event) return ESP_ERR_INVALID_ARG;
    return claw_event_router_publish(event);
}

static void deep_sleep_cb(void *user_ctx)
{
    (void)user_ctx;
    rover_display_set_state(ROVER_DISPLAY_STATE_SLEEPING);
    /* Best-effort UI refresh, then halt rovers and sleep. */
    vTaskDelay(pdMS_TO_TICKS(100));
    rover_board_display_sleep();
    /* Wake on BtnA (GPIO37) low. */
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_37, 0);
    esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_39, ESP_EXT1_WAKEUP_ALL_LOW);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_deep_sleep_start();
}

static void wifi_state_cb(bool connected, void *user_ctx)
{
    (void)user_ctx;
    rover_display_set_state(connected ? ROVER_DISPLAY_STATE_IDLE
                                      : ROVER_DISPLAY_STATE_OFFLINE);
}

esp_err_t app_rover_start(const rover_demo_settings_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;

    ESP_RETURN_ON_ERROR(init_paths(&s_paths), TAG, "paths");

    /* Event router: route every message to the agent by default. */
    claw_event_router_config_t router_cfg = {
        .rules_path = s_paths.router_rules_path,
        .task_stack_size = 8 * 1024,
        .task_priority = 5,
        .task_core = tskNO_AFFINITY,
        .core_submit_timeout_ms = 1000,
        .core_receive_timeout_ms = 130000,
        .default_route_messages_to_agent = (s->llm_api_key[0] && s->llm_model[0]),
        .session_builder = cap_session_mgr_build_session_id,
    };
    ESP_RETURN_ON_ERROR(cap_session_mgr_set_session_root_dir(s_paths.memory_session_root),
                        TAG, "session root");
    ESP_RETURN_ON_ERROR(claw_event_router_init(&router_cfg), TAG, "event router");

    /* Memory (lightweight). */
    claw_memory_config_t mem_cfg = {
        .session_root_dir = s_paths.memory_session_root,
        .memory_root_dir  = s_paths.memory_root_dir,
        .max_session_messages = 20,
        .max_message_chars = 1024,
        .llm = {
            .api_key      = s->llm_api_key,
            .backend_type = s->llm_backend_type,
            .profile      = s->llm_profile,
            .model        = s->llm_model,
            .base_url     = s->llm_base_url,
            .auth_type    = s->llm_auth_type,
            .timeout_ms   = (uint32_t)strtoul(s->llm_timeout_ms, NULL, 10),
            .image_max_bytes = 0,
        },
        .enable_async_extract_stage_note = false,
    };
    ESP_RETURN_ON_ERROR(claw_memory_init(&mem_cfg), TAG, "memory");

    /* Skills */
    claw_skill_config_t skill_cfg = {
        .skills_root_dir = s_paths.skills_root_dir,
        .session_state_root_dir = s_paths.memory_session_root,
        .max_file_bytes = 10 * 1024,
    };
    ESP_RETURN_ON_ERROR(claw_skill_init(&skill_cfg), TAG, "skill");

    /* Capabilities */
    ESP_RETURN_ON_ERROR(claw_cap_init(), TAG, "cap_init");

    /* cap_rover */
    cap_rover_config_t rover_cfg = {
        .i2c_port = 1,
        .sda_gpio = 0,
        .scl_gpio = 26,
        .i2c_freq_hz = 100000,
        .rover_addr = 0x38,
        .gripper_servo_idx = 1,
        .gripper_open_angle = 35,
        .gripper_close_angle = 150,
        .hw_task_stack_size = 4096,
        .hw_task_priority = 5,
        .hw_task_core = 0,
    };
    ESP_RETURN_ON_ERROR(cap_rover_init(&rover_cfg), TAG, "cap_rover");
    cap_rover_set_imu_read(imu_read_adapter);
    ESP_RETURN_ON_ERROR(cap_rover_register_group(), TAG, "cap_rover register");

    /* cap_unitv */
    cap_unitv_config_t unitv_cfg = {
        .uart_port = 1,
        .tx_gpio = 32,
        .rx_gpio = 33,
        .baud_rate = 115200,
        .rx_buffer_bytes = 4096,
        .default_timeout_ms = 7000,
        .capture_timeout_ms = 12000,
        .max_jpeg_bytes = 40960,
    };
    ESP_RETURN_ON_ERROR(cap_unitv_init(&unitv_cfg), TAG, "cap_unitv");
    cap_unitv_vision_config_t vision_cfg = {
        .api_key      = s->llm_api_key,
        .model        = s->llm_model,
        .base_url     = s->llm_base_url,
        .backend_type = s->llm_backend_type,
        .auth_type    = s->llm_auth_type,
        .timeout_ms   = (uint32_t)strtoul(s->llm_timeout_ms, NULL, 10),
        .max_response_tokens = 256,
    };
    cap_unitv_set_vision_config(&vision_cfg);
    ESP_RETURN_ON_ERROR(cap_unitv_register_group(), TAG, "cap_unitv register");

    /* cap_im_tg */
    if (s->tg_bot_token[0]) {
        ESP_RETURN_ON_ERROR(cap_im_tg_set_token(s->tg_bot_token), TAG, "tg token");
    }
    ESP_RETURN_ON_ERROR(cap_im_tg_set_attachment_config(&(cap_im_tg_attachment_config_t){
                            .storage_root_dir = s_paths.im_attachment_root,
                            .max_inbound_file_bytes = 1 * 1024 * 1024,
                            .enable_inbound_attachments = false,
                        }), TAG, "tg attachments");
    ESP_RETURN_ON_ERROR(cap_im_tg_register_group(), TAG, "cap_im_tg register");

    /* Other esp-claw caps */
    ESP_RETURN_ON_ERROR(cap_files_set_base_dir(rover_demo_fatfs_base_path), TAG, "cap_files");
    ESP_RETURN_ON_ERROR(cap_files_register_group(), TAG, "cap_files register");
    ESP_RETURN_ON_ERROR(cap_skill_mgr_register_group(), TAG, "cap_skill register");
    ESP_RETURN_ON_ERROR(cap_system_register_group(), TAG, "cap_system register");
    ESP_RETURN_ON_ERROR(cap_time_register_group(), TAG, "cap_time register");
    ESP_RETURN_ON_ERROR(cap_session_mgr_register_group(), TAG, "cap_session_mgr register");

    ESP_RETURN_ON_ERROR(claw_cap_set_llm_visible_groups(
                            ROVER_LLM_VISIBLE_GROUPS,
                            sizeof(ROVER_LLM_VISIBLE_GROUPS) / sizeof(ROVER_LLM_VISIBLE_GROUPS[0])),
                        TAG, "llm visible groups");
    ESP_RETURN_ON_ERROR(claw_cap_start_all(), TAG, "cap_start");

    /* Telegram outbound binding */
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("telegram", "tg_send_message"),
                        TAG, "tg outbound");

    /* Optional: register CLI for cap_rover / cap_unitv if console is up. */
    cap_rover_register_cli();
    cap_unitv_register_cli();

    /* claw_core (LLM agent loop) */
    bool llm_enabled = (s->llm_api_key[0] && s->llm_model[0] && s->llm_profile[0]);
    if (llm_enabled) {
        claw_core_config_t core_cfg = {
            .api_key = s->llm_api_key,
            .backend_type = s->llm_backend_type,
            .profile = s->llm_profile,
            .model = s->llm_model,
            .base_url = s->llm_base_url,
            .auth_type = s->llm_auth_type,
            .timeout_ms = (uint32_t)strtoul(s->llm_timeout_ms, NULL, 10),
            .system_prompt = ROVER_SYSTEM_PROMPT,
            .append_session_turn = claw_memory_append_session_turn_callback,
            .call_cap = claw_cap_call_from_core,
            .task_stack_size = 16 * 1024,
            .task_priority = 5,
            .task_core = tskNO_AFFINITY,
            .max_tool_iterations = 20,
            .request_queue_len = 4,
            .response_queue_len = 4,
            .max_context_providers = 8,
        };
        ESP_RETURN_ON_ERROR(claw_core_init(&core_cfg), TAG, "claw_core");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_profile_provider),         TAG, "ctx profile");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_long_term_lightweight_provider), TAG, "ctx mem lite");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_session_history_provider), TAG, "ctx history");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_skill_skills_list_provider),      TAG, "ctx skills list");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_skill_active_skill_docs_provider),TAG, "ctx active skills");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_cap_tools_provider),              TAG, "ctx tools");
        ESP_RETURN_ON_ERROR(claw_core_start(), TAG, "claw_core start");
    } else {
        ESP_LOGW(TAG, "LLM not configured — agent disabled. Configure API key/model.");
    }

    ESP_RETURN_ON_ERROR(claw_event_router_start(), TAG, "event router start");

    /* Buttons + display refresh. */
    rover_buttons_config_t btn_cfg = {
        .on_btn_a_short = demo_event_cb,
        .on_btn_a_long  = deep_sleep_cb,
    };
    ESP_RETURN_ON_ERROR(rover_buttons_init(&btn_cfg), TAG, "buttons");

    /* Wire WiFi state changes to display. */
    rover_demo_wifi_register_state_cb(wifi_state_cb, NULL);

    /* Initial display state */
    rover_display_set_state(rover_demo_wifi_is_connected()
                            ? ROVER_DISPLAY_STATE_IDLE
                            : ROVER_DISPLAY_STATE_OFFLINE);

    return ESP_OK;
}
```

- [ ] **Step 3: Commit**

```bash
git add application/rover_demo/main/app_rover.h \
        application/rover_demo/main/app_rover.c
git commit -m "feat(rover_demo): wire claw_core, capabilities, and buttons in app_rover"
```

---

## Task 12: main.c, FATFS content, and full build verification

**Files:**
- Create: `application/rover_demo/main/main.c`
- Create: `application/rover_demo/fatfs_image/memory/identity.md`
- Create: `application/rover_demo/fatfs_image/memory/soul.md`
- Create: `application/rover_demo/fatfs_image/memory/user.md`
- Create: `application/rover_demo/fatfs_image/memory/MEMORY.md`
- Create: `application/rover_demo/fatfs_image/memory/memory_records.jsonl`
- Create: `application/rover_demo/fatfs_image/memory/memory_index.json`
- Create: `application/rover_demo/fatfs_image/memory/memory_digest.log`
- Create: `application/rover_demo/fatfs_image/router_rules/router_rules.json`
- Create: `application/rover_demo/fatfs_image/skills/skills_list.json`
- Create: `application/rover_demo/fatfs_image/skills/rover_ops.md`
- Create: `application/rover_demo/fatfs_image/skills/rover_search.md`

- [ ] **Step 1: Create `main/main.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include "app_rover.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "rover_demo_settings.h"
#include "rover_demo_wifi.h"
#include "rover_display.h"
#include "setup_device.h"
#include "wear_levelling.h"

static const char *TAG = "rover_demo_main";
static rover_demo_settings_t s_settings = {0};
static wl_handle_t s_wl = WL_INVALID_HANDLE;

#define FATFS_PARTITION_LABEL "storage"

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_fatfs(void)
{
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    return esp_vfs_fat_spiflash_mount_rw_wl(rover_demo_fatfs_base_path,
                                            FATFS_PARTITION_LABEL,
                                            &mount_cfg, &s_wl);
}

static void apply_timezone(const char *tz)
{
    if (!tz || !tz[0]) return;
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "timezone set: %s", tz);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting rover_demo");
    ESP_ERROR_CHECK(init_nvs());

    ESP_ERROR_CHECK(rover_demo_settings_init());
    ESP_ERROR_CHECK(rover_demo_settings_load(&s_settings));
    apply_timezone(s_settings.time_timezone);

    ESP_ERROR_CHECK(rover_board_init());
    rover_display_set_state(ROVER_DISPLAY_STATE_BOOT);

    esp_err_t fs_err = init_fatfs();
    if (fs_err != ESP_OK) {
        ESP_LOGE(TAG, "FATFS mount failed: %s", esp_err_to_name(fs_err));
        ESP_ERROR_CHECK(fs_err);
    }

    ESP_ERROR_CHECK(rover_demo_wifi_init());
    if (s_settings.wifi_ssid[0]) {
        esp_err_t wifi_err = rover_demo_wifi_start(s_settings.wifi_ssid, s_settings.wifi_password);
        if (wifi_err == ESP_OK) {
            esp_err_t conn = rover_demo_wifi_wait_connected(30000);
            if (conn != ESP_OK) {
                ESP_LOGW(TAG, "WiFi STA connect timeout — continuing offline");
            }
        } else {
            ESP_LOGW(TAG, "WiFi start failed: %s", esp_err_to_name(wifi_err));
        }
    } else {
        ESP_LOGW(TAG, "WiFi SSID not configured — offline mode");
    }

    ESP_ERROR_CHECK(app_rover_start(&s_settings));
}
```

- [ ] **Step 2: Create FATFS memory files**

`fatfs_image/memory/identity.md`:
```markdown
Ты AI Rover. Ты физический робот на колёсах Mecanum с захватом и камерой.
Ты находишься в реальном мире. Твои действия имеют физические последствия.
Будь точен в командах движения.
```

`fatfs_image/memory/soul.md`:
```markdown
Ты дружелюбный, краток в ответах, и любопытен к окружающему миру через камеру.
Ты не задаёшь лишних разрешений — выполняешь команды пользователя сразу.
```

`fatfs_image/memory/user.md`:
```markdown
Пользователь — владелец ровера. Пишет тебе через Telegram.
```

`fatfs_image/memory/MEMORY.md` (empty):
```
```

`fatfs_image/memory/memory_records.jsonl` (empty):
```
```

`fatfs_image/memory/memory_index.json`:
```json
{"summaries":{},"version":1}
```

`fatfs_image/memory/memory_digest.log` (empty):
```
```

- [ ] **Step 3: Create `fatfs_image/router_rules/router_rules.json`**

```json
{
  "version": 1,
  "rules": [
    {
      "id": "default_to_agent",
      "enabled": true,
      "consume_on_match": true,
      "match": {"event_type": "message"},
      "actions": [{"kind": "run_agent"}]
    }
  ]
}
```

- [ ] **Step 4: Create `fatfs_image/skills/skills_list.json`**

```json
{
  "skills": [
    {
      "id": "rover_ops",
      "file": "rover_ops.md",
      "summary": "Multi-step movement patterns and trajectory composition.",
      "cap_groups": ["cap_rover"]
    },
    {
      "id": "rover_search",
      "file": "rover_search.md",
      "summary": "Sweep-and-detect search pattern using rover_turn + unitv_scan.",
      "cap_groups": ["cap_rover", "cap_unitv"]
    }
  ]
}
```

- [ ] **Step 5: Create `fatfs_image/skills/rover_ops.md`**

```markdown
# Rover Ops

Multi-step movement patterns. Activate this skill when the user asks for
trajectories that need more than one `rover_move` or `rover_turn` call.

## Patterns

### Square loop

To trace a square of side ~1 second forward at speed 60:
1. `rover_move(x=0, y=60, duration_ms=1000)`
2. `rover_turn(direction="left", angle_deg=90, speed_percent=50)`
3. Repeat 3 more times.

Observe the result of each call. If any returns `emergency_stop`, abort
the sequence and report.

### Approach + grab

1. `unitv_scan` to confirm target presence.
2. `rover_move` toward the target in short hops (500-1000ms each), checking
   the scene between hops with `unitv_scan`.
3. Stop ~1 hop short of contact.
4. `rover_gripper_open`.
5. `rover_move(x=0, y=40, duration_ms=400)` to nudge in.
6. `rover_gripper_close`.
7. Backup with `rover_move(x=0, y=-50, duration_ms=600)`.

## Error handling

- `emergency_stop`: do not retry. Acknowledge and ask the user.
- `timeout` on `rover_turn`: motors may be obstructed or IMU disabled.
  Try a smaller angle or `rover_move` instead.
- `imu_unavailable`: fall back to `rover_move(z=...)` for rotation, but
  expect lower accuracy.
```

- [ ] **Step 6: Create `fatfs_image/skills/rover_search.md`**

```markdown
# Rover Search

Find a named object using the camera. Activate this skill when the user
asks to look for something specific (e.g., "find the red mug").

## Procedure

1. **Initial check.** Call `unitv_scan` once. If it reports the target
   matches a generic class (face, person), you may be done.

2. **Detailed look.** If `unitv_scan` is uncertain, call
   `unitv_capture(question="Is there a <target> visible? If yes, where:
   left, center, or right?")`. Use the analysis text to decide whether
   to act.

3. **Sweep.** If not found in the current view, sweep:
   - `rover_turn(direction="left", angle_deg=45, speed_percent=40)`
   - `unitv_scan` (or capture if class is non-generic)
   - Repeat up to 8 times (one full 360° sweep).
   - Stop at the first sighting.

4. **Confirm.** Once a candidate is detected, do one
   `unitv_capture(question="Confirm there is a <target>")` to reduce
   false positives.

## Limits

- Maximum 12 vision tool calls per user turn. Beyond that, ask the user
  to clarify or move the rover manually.
- Do not loop search and movement — that risks driving into walls.
- The camera is fixed; only `rover_turn` changes the view direction.
```

- [ ] **Step 7: Run the full build**

```bash
cd application/rover_demo
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py build
```

Expected: Build completes successfully, producing `build/rover_demo.bin` and a `build/storage.bin` from the FATFS image. Warnings about unused parameters in capability callbacks are acceptable. Errors that need fixing:

- Missing M5Unified component → re-run `idf.py reconfigure` after `idf_component.yml` is in place.
- Linker errors about `cap_im_feishu_*`, `cap_im_qq_*`, etc. → these caps are not in REQUIRES, the linker should not see them. If they appear, double-check the `main/CMakeLists.txt` REQUIRES list.

- [ ] **Step 8: Smoke-test plan (manual, on hardware)**

After flashing (`idf.py -p /dev/ttyUSB0 flash monitor`):

1. Boot — display shows "BOOT" then "OFFLINE" or "IDLE" depending on WiFi.
2. Open serial monitor → run console commands to test hardware:
   - `rover_move -y 50 -d 1000` — rover should move forward 1 second.
   - `rover_stop` — motors zero.
   - `rover_open` / `rover_close` — gripper actuates.
   - `unitv_scan` — should print SCAN response from UnitV.
   - `unitv_capture` — should print "capture: NNNN bytes".
3. Send "вперёд 2 секунды" via Telegram → rover moves, sends ok reply.
4. Send "что ты видишь?" via Telegram → vision_capture runs, response in chat.
5. Press BtnB during a long rover_move → rover stops, response shows
   `emergency_stop` status.
6. Hold BtnA for 3+ seconds → rover enters deep sleep. Press BtnA to wake.

- [ ] **Step 9: Commit**

```bash
git add application/rover_demo/main/main.c \
        application/rover_demo/fatfs_image/
git commit -m "feat(rover_demo): add app_main, FATFS seed content, and skills"
```

---

## Self-Review

**Spec coverage check:**

- ✅ Section 1 (Architecture): event router → claw_core → caps → outbound binding wired in app_rover.c (Task 11).
- ✅ Section 2.1 (cap_rover): all 6 capabilities + hardware task pattern + emergency stop (Tasks 1-3).
- ✅ Section 2.2 (cap_unitv): unitv_scan, unitv_capture, vision LLM call with same credentials as claw_core (Tasks 4-6).
- ✅ Section 3 (App structure): scaffold + board support + main + FATFS (Tasks 7, 8, 12).
- ✅ Section 4 (Memory strategy): lightweight providers registered in app_rover.c (Task 11).
- ✅ Section 5 (System prompt + skills): system prompt in app_rover.c, identity/soul/user + rover_ops + rover_search in fatfs_image (Tasks 11, 12).
- ✅ Section 6 (Display, buttons, error handling): rover_display.cpp, rover_buttons.c, deep_sleep_cb in app_rover.c (Tasks 8, 10, 11).

**Placeholder scan:** No TBDs, TODOs, or "implement later" lines. Each step has either complete code, a complete file content, or an explicit command + expected output. Spec section 6's "rover_buttons_task" is implemented as `buttons_task` in Task 10 with the documented 20ms cadence.

**Type consistency:** `cap_rover_imu_read_fn` (Task 1) signature matches `imu_read_adapter` and `rover_board_imu_read` (Tasks 8, 11). `cap_unitv_vision_config_t` field names match between header (Task 4), implementation (Task 4 set_vision_config), and caller (Task 11). `claw_event_t` field initialization in `inject_demo_event` (Task 10) uses fields from the `claw_event_router/include/claw_event.h` header with `event_id`, `source_cap`, `event_type`, `chat_id`, `sender_id`, `content_type`, `text` — matches the existing esp-claw structure.

**Known limitations / things to verify on hardware:**

1. UnitV-M12 protocol assumed: JSON-line commands and the `{ok, result.size}` + binary JPEG response format. If the actual V-Function firmware on the camera differs, `cap_unitv_uart.c` will need its protocol parser adjusted — but the surrounding code (capability registration, vision LLM call) does not change.
2. M5Unified pin defaults for M5StickC Plus assume the default `M5.config()` populates the right pins for the internal IMU and AXP192. If the M5Unified version pulled by Component Manager does not auto-detect M5StickC Plus, `cfg.fallback_board = m5::board_t::board_M5StickCPlus` may need to be set in `setup_device.cpp`.
3. `claw_event_router_publish` from `rover_buttons.c` expects the event_router to have been started; this is guaranteed by initialization order in `app_rover.c` (router_start is before `rover_buttons_init`).
4. The 4MB partition layout leaves no OTA partition. To add OTA later, swap `factory` for `ota_0` + `ota_1`, each ~1MB, and shrink FATFS to ~1.5MB.
