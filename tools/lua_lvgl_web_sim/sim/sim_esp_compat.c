#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>
#include <lua.h>
#include "lvgl.h"

#include "cap_lua.h"
#include "display_arbiter.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct sim_semaphore {
    int count;
    bool mutex;
};

struct sim_esp_timer {
    esp_timer_cb_t callback;
    void *arg;
    uint64_t period_us;
    int64_t next_fire_us;
    bool active;
};

static display_arbiter_owner_t s_display_owner = DISPLAY_ARBITER_OWNER_NONE;
static esp_lcd_panel_io_callbacks_t s_io_callbacks;
static void *s_io_user_ctx;
static struct sim_esp_timer *s_timers[16];
static int s_timer_count;
static uint8_t *s_canvas_rgba;
static int s_canvas_width;
static int s_canvas_height;
static size_t s_canvas_rgba_size;

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    default: return "ESP_ERR_UNKNOWN";
    }
}

display_arbiter_owner_t display_arbiter_get_owner(void)
{
    return s_display_owner;
}

esp_err_t display_arbiter_acquire(display_arbiter_owner_t owner)
{
    if (s_display_owner != DISPLAY_ARBITER_OWNER_NONE && s_display_owner != owner) {
        return ESP_ERR_INVALID_STATE;
    }
    s_display_owner = owner;
    return ESP_OK;
}

esp_err_t display_arbiter_release(display_arbiter_owner_t owner)
{
    if (s_display_owner != owner) {
        return ESP_ERR_INVALID_STATE;
    }
    s_display_owner = DISPLAY_ARBITER_OWNER_NONE;
    return ESP_OK;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct sim_semaphore *sem = (struct sim_semaphore *)calloc(1, sizeof(*sem));
    if (sem) {
        sem->count = 1;
        sem->mutex = true;
    }
    return sem;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return (SemaphoreHandle_t)calloc(1, sizeof(struct sim_semaphore));
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)ticks;
    if (!sem || sem->count <= 0) {
        return pdFALSE;
    }
    sem->count--;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    if (!sem) {
        return pdFALSE;
    }
    sem->count = sem->mutex ? 1 : sem->count + 1;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *high_task_woken)
{
    if (high_task_woken) {
        *high_task_woken = pdFALSE;
    }
    return xSemaphoreGive(sem);
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)(emscripten_get_now() * 1000.0);
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out_handle)
{
    struct sim_esp_timer *timer;
    if (!args || !args->callback || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    timer = (struct sim_esp_timer *)calloc(1, sizeof(*timer));
    if (!timer) {
        return ESP_ERR_NO_MEM;
    }
    timer->callback = args->callback;
    timer->arg = args->arg;
    *out_handle = timer;
    if (s_timer_count < (int)(sizeof(s_timers) / sizeof(s_timers[0]))) {
        s_timers[s_timer_count++] = timer;
    }
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t handle, uint64_t period_us)
{
    if (!handle || period_us == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->period_us = period_us;
    handle->next_fire_us = esp_timer_get_time() + (int64_t)period_us;
    handle->active = true;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->active = false;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_timer_count; i++) {
        if (s_timers[i] == handle) {
            memmove(&s_timers[i], &s_timers[i + 1], (size_t)(s_timer_count - i - 1) * sizeof(s_timers[0]));
            s_timer_count--;
            break;
        }
    }
    free(handle);
    return ESP_OK;
}

static void sim_pump_timers(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_timer_count; i++) {
        struct sim_esp_timer *timer = s_timers[i];
        if (!timer || !timer->active || now < timer->next_fire_us) {
            continue;
        }
        timer->callback(timer->arg);
        do {
            timer->next_fire_us += (int64_t)timer->period_us;
        } while (now >= timer->next_fire_us);
    }
}

static esp_err_t sim_ensure_canvas_rgba(int width, int height)
{
    size_t needed;
    uint8_t *next;

    if (width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    needed = (size_t)width * (size_t)height * 4u;
    if (s_canvas_rgba && s_canvas_width == width && s_canvas_height == height && s_canvas_rgba_size >= needed) {
        return ESP_OK;
    }

    next = (uint8_t *)realloc(s_canvas_rgba, needed);
    if (!next) {
        return ESP_ERR_NO_MEM;
    }

    s_canvas_rgba = next;
    s_canvas_width = width;
    s_canvas_height = height;
    s_canvas_rgba_size = needed;
    memset(s_canvas_rgba, 0, needed);
    return ESP_OK;
}

static void sim_copy_xrgb8888_to_rgba(int x, int y, int width, int height, const uint8_t *src)
{
    for (int row = 0; row < height; row++) {
        const uint8_t *src_row = src + (size_t)row * (size_t)width * 4u;
        uint8_t *dst = s_canvas_rgba + ((size_t)(y + row) * (size_t)s_canvas_width + (size_t)x) * 4u;

        for (int col = 0; col < width; col++) {
            dst[0] = src_row[2];
            dst[1] = src_row[1];
            dst[2] = src_row[0];
            dst[3] = 255;
            src_row += 4;
            dst += 4;
        }
    }
}

void sim_esp_compat_pump_once(void)
{
    sim_pump_timers();
    if (lv_is_initialized()) {
        (void)lv_timer_handler();
    }
}

BaseType_t xTaskCreate(TaskFunction_t task,
                       const char *name,
                       uint32_t stack_depth,
                       void *arg,
                       UBaseType_t priority,
                       TaskHandle_t *out_handle)
{
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)arg;
    (void)priority;
    if (out_handle) {
        *out_handle = NULL;
    }
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    sim_esp_compat_pump_once();
    if (ticks > 0) {
        emscripten_sleep((unsigned int)ticks);
    }
    sim_esp_compat_pump_once();
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return NULL;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait)
{
    (void)clear_on_exit;
    (void)ticks_to_wait;
    return 1;
}

void xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
}

esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io,
                                                    const esp_lcd_panel_io_callbacks_t *callbacks,
                                                    void *user_ctx)
{
    (void)io;
    if (callbacks) {
        s_io_callbacks = *callbacks;
    } else {
        memset(&s_io_callbacks, 0, sizeof(s_io_callbacks));
    }
    s_io_user_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel,
                                    int x_start,
                                    int y_start,
                                    int x_end,
                                    int y_end,
                                    const void *color_data)
{
    (void)panel;
    int width = x_end - x_start;
    int height = y_end - y_start;
    int canvas_width;
    int canvas_height;
    double convert_start;
    double convert_ms;
    esp_err_t err;

    if (!color_data || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    canvas_width = EM_ASM_INT({ return Module.canvas ? (Module.canvas.width | 0) : 0; });
    canvas_height = EM_ASM_INT({ return Module.canvas ? (Module.canvas.height | 0) : 0; });
    err = sim_ensure_canvas_rgba(canvas_width, canvas_height);
    if (err != ESP_OK) {
        return err;
    }
    if (x_start < 0 || y_start < 0 || x_end > canvas_width || y_end > canvas_height) {
        return ESP_ERR_INVALID_ARG;
    }

    convert_start = emscripten_get_now();
    sim_copy_xrgb8888_to_rgba(x_start, y_start, width, height, (const uint8_t *)color_data);
    convert_ms = emscripten_get_now() - convert_start;

    EM_ASM({
        const perfNow = (typeof performance !== 'undefined' && performance.now) ?
            () => performance.now() :
            () => Date.now();
        const canvas = Module.canvas;
        const ctx = canvas.getContext('2d');
        const cw = canvas.width | 0;
        const ch = canvas.height | 0;
        const x = $0;
        const y = $1;
        const w = $2;
        const h = $3;
        const framePtr = $4;
        const convertMs = $5;
        if (!Module.__lvglPerf) {
            const now = perfNow();
            Module.__lvglPerf = {};
            if (typeof window !== 'undefined') {
                window.__lvglPerf = Module.__lvglPerf;
            }
            Module.__lvglPerf.flushCalls = 0;
            Module.__lvglPerf.flushPixels = 0;
            Module.__lvglPerf.convertMs = 0;
            Module.__lvglPerf.copyMs = 0;
            Module.__lvglPerf.rafFrames = 0;
            Module.__lvglPerf.putMs = 0;
            Module.__lvglPerf.presentPixels = 0;
            Module.__lvglPerf.lastFps = 0;
            Module.__lvglPerf.lastFrameMs = 0;
            Module.__lvglPerf.resetAt = now;
            Module.__lvglPerf.logAt = now;
            Module.__lvglPerf.logFrames = 0;
            Module.__lvglPerf.logConvertMs = 0;
            Module.__lvglPerf.logCopyMs = 0;
            Module.__lvglPerf.logPutMs = 0;
            Module.__lvglPerf.logPixels = 0;
            canvas.dataset.lvglPerf = JSON.stringify(Module.__lvglPerf);
        }
        const perf = Module.__lvglPerf;
        if (!Module.__lvglCanvas || Module.__lvglCanvas.w !== cw || Module.__lvglCanvas.h !== ch) {
            const image = ctx.createImageData(cw, ch);
            Module.__lvglCanvas = {};
            Module.__lvglCanvas.w = cw;
            Module.__lvglCanvas.h = ch;
            Module.__lvglCanvas.image = image;
            Module.__lvglCanvas.dirty = null;
            Module.__lvglCanvas.pending = false;
        }
        const target = Module.__lvglCanvas;
        perf.flushCalls++;
        perf.flushPixels += w * h;
        perf.convertMs += convertMs;
        perf.logConvertMs += convertMs;
        const x2 = x + w;
        const y2 = y + h;
        if (target.dirty) {
            target.dirty.x1 = Math.min(target.dirty.x1, x);
            target.dirty.y1 = Math.min(target.dirty.y1, y);
            target.dirty.x2 = Math.max(target.dirty.x2, x2);
            target.dirty.y2 = Math.max(target.dirty.y2, y2);
        } else {
            target.dirty = {};
            target.dirty.x1 = x;
            target.dirty.y1 = y;
            target.dirty.x2 = x2;
            target.dirty.y2 = y2;
        }
        if (!target.pending) {
            target.pending = true;
            requestAnimationFrame(() => {
                target.pending = false;
                const d = target.dirty;
                target.dirty = null;
                if (!d) return;
                const dx = Math.max(0, d.x1 | 0);
                const dy = Math.max(0, d.y1 | 0);
                const dw = Math.min(target.w, d.x2 | 0) - dx;
                const dh = Math.min(target.h, d.y2 | 0) - dy;
                if (dw > 0 && dh > 0) {
                    const copyStart = perfNow();
                    const bytesPerRow = dw * 4;
                    for (let row = 0; row < dh; row++) {
                        const srcStart = framePtr + (((dy + row) * target.w + dx) * 4);
                        const dstStart = (((dy + row) * target.w + dx) * 4);
                        target.image.data.set(HEAPU8.subarray(srcStart, srcStart + bytesPerRow), dstStart);
                    }
                    const copyMs = perfNow() - copyStart;
                    const putStart = perfNow();
                    ctx.putImageData(target.image, 0, 0, dx, dy, dw, dh);
                    const putMs = perfNow() - putStart;
                    perf.copyMs += copyMs;
                    perf.putMs += putMs;
                    perf.presentPixels += dw * dh;
                    perf.rafFrames++;
                    perf.logFrames++;
                    perf.logCopyMs += copyMs;
                    perf.logPutMs += putMs;
                    perf.logPixels += dw * dh;
                    const now = perfNow();
                    const elapsed = now - perf.logAt;
                    if (elapsed >= 1000) {
                        perf.lastFps = perf.logFrames * 1000 / elapsed;
                        perf.lastFrameMs = elapsed / perf.logFrames;
                        if (Module.__lvglPerfLog || new URLSearchParams(location.search).get('perf') === '1') {
                            const msg = `[perf] fps=${perf.lastFps.toFixed(1)} frame=${perf.lastFrameMs.toFixed(2)}ms ` +
                                `convert=${(perf.logConvertMs / perf.logFrames).toFixed(2)}ms ` +
                                `copy=${(perf.logCopyMs / perf.logFrames).toFixed(2)}ms ` +
                                `put=${(perf.logPutMs / perf.logFrames).toFixed(2)}ms ` +
                                `pixels=${Math.round(perf.logPixels / perf.logFrames)}`;
                            if (Module.print) Module.print(msg);
                            else console.log(msg);
                        }
                        perf.logAt = now;
                        perf.logFrames = 0;
                        perf.logConvertMs = 0;
                        perf.logCopyMs = 0;
                        perf.logPutMs = 0;
                        perf.logPixels = 0;
                    }
                    canvas.dataset.lvglPerf = JSON.stringify(perf);
                }
            });
        }
    }, x_start, y_start, width, height, s_canvas_rgba, convert_ms);
    if (s_io_callbacks.on_color_trans_done) {
        esp_lcd_panel_io_event_data_t edata = {0};
        s_io_callbacks.on_color_trans_done(NULL, &edata, s_io_user_ctx);
    }
    return ESP_OK;
}

esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp)
{
    (void)tp;
    EM_ASM({
        if (Module.__takeTouchEvent) {
            Module.__takeTouchEvent();
        }
    });
    return ESP_OK;
}

esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t tp,
                                 esp_lcd_touch_point_data_t *point,
                                 uint8_t *point_count,
                                 uint8_t max_point_count)
{
    (void)tp;
    if (!point || !point_count || max_point_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int pressed = EM_ASM_INT({ return Module.__touchState && Module.__touchState.pressed ? 1 : 0; });
    if (!pressed) {
        *point_count = 0;
        return ESP_OK;
    }
    point[0].x = (uint16_t)EM_ASM_INT({ return Module.__touchState.x | 0; });
    point[0].y = (uint16_t)EM_ASM_INT({ return Module.__touchState.y | 0; });
    point[0].strength = 1;
    *point_count = 1;
    return ESP_OK;
}

esp_err_t cap_lua_register_module(const char *name, lua_CFunction openf)
{
    (void)name;
    (void)openf;
    return ESP_OK;
}

esp_err_t cap_lua_register_exit_cleanup(void (*cleanup)(lua_State *L))
{
    (void)cleanup;
    return ESP_OK;
}

bool cap_lua_runtime_stop_requested(lua_State *L)
{
    (void)L;
    return EM_ASM_INT({ return Module.__stopRequested ? 1 : 0; }) != 0;
}
