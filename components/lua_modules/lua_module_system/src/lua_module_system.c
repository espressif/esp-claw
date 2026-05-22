/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_system.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "cap_lua.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"

#if (configUSE_TRACE_FACILITY == 1)
static const char *lua_module_system_task_state_name(eTaskState state)
{
    switch (state) {
    case eRunning:
        return "running";
    case eReady:
        return "ready";
    case eBlocked:
        return "blocked";
    case eSuspended:
        return "suspended";
    case eDeleted:
        return "deleted";
    case eInvalid:
    default:
        return "invalid";
    }
}
#endif

static void lua_module_system_push_heap_caps_constants(lua_State *L)
{
    lua_pushinteger(L, MALLOC_CAP_DEFAULT);
    lua_setfield(L, -2, "DEFAULT");
    lua_pushinteger(L, MALLOC_CAP_INTERNAL);
    lua_setfield(L, -2, "INTERNAL");
    lua_pushinteger(L, MALLOC_CAP_SPIRAM);
    lua_setfield(L, -2, "SPIRAM");
    lua_pushinteger(L, MALLOC_CAP_DMA);
    lua_setfield(L, -2, "DMA");
    lua_pushinteger(L, MALLOC_CAP_8BIT);
    lua_setfield(L, -2, "BIT8");
    lua_pushinteger(L, MALLOC_CAP_32BIT);
    lua_setfield(L, -2, "BIT32");
    lua_pushinteger(L, MALLOC_CAP_EXEC);
    lua_setfield(L, -2, "EXEC");
    lua_pushinteger(L, MALLOC_CAP_IRAM_8BIT);
    lua_setfield(L, -2, "IRAM_8BIT");
    lua_pushinteger(L, MALLOC_CAP_RTCRAM);
    lua_setfield(L, -2, "RTCRAM");
    lua_pushinteger(L, MALLOC_CAP_RETENTION);
    lua_setfield(L, -2, "RETENTION");
}

static int lua_module_system_heap_get_info(lua_State *L)
{
    lua_Integer caps_value = luaL_optinteger(L, 1, MALLOC_CAP_DEFAULT);
    multi_heap_info_t info = {0};
    uint32_t caps = (uint32_t)caps_value;

    heap_caps_get_info(&info, caps);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)caps);
    lua_setfield(L, -2, "caps");
    lua_pushinteger(L, (lua_Integer)heap_caps_get_total_size(caps));
    lua_setfield(L, -2, "total_size");
    lua_pushinteger(L, (lua_Integer)info.total_free_bytes);
    lua_setfield(L, -2, "free_size");
    lua_pushinteger(L, (lua_Integer)info.total_allocated_bytes);
    lua_setfield(L, -2, "allocated_size");
    lua_pushinteger(L, (lua_Integer)info.minimum_free_bytes);
    lua_setfield(L, -2, "minimum_free_size");
    lua_pushinteger(L, (lua_Integer)info.largest_free_block);
    lua_setfield(L, -2, "largest_free_block");
    lua_pushinteger(L, (lua_Integer)info.allocated_blocks);
    lua_setfield(L, -2, "allocated_blocks");
    lua_pushinteger(L, (lua_Integer)info.free_blocks);
    lua_setfield(L, -2, "free_blocks");
    lua_pushinteger(L, (lua_Integer)info.total_blocks);
    lua_setfield(L, -2, "total_blocks");
    return 1;
}

static int lua_module_system_heap_get_task_watermarks(lua_State *L)
{
#if (configUSE_TRACE_FACILITY == 1)
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = NULL;
    UBaseType_t count = 0;

    tasks = calloc((size_t)task_count + 4, sizeof(*tasks));
    if (!tasks) {
        return luaL_error(L, "system.heap.get_task_watermarks: out of memory");
    }

    count = uxTaskGetSystemState(tasks, task_count + 4, NULL);
    lua_newtable(L);
    for (UBaseType_t i = 0; i < count; i++) {
        lua_newtable(L);
        lua_pushstring(L, tasks[i].pcTaskName ? tasks[i].pcTaskName : "(unnamed)");
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)tasks[i].xTaskNumber);
        lua_setfield(L, -2, "task_number");
        lua_pushstring(L, lua_module_system_task_state_name(tasks[i].eCurrentState));
        lua_setfield(L, -2, "state");
        lua_pushinteger(L, (lua_Integer)tasks[i].uxCurrentPriority);
        lua_setfield(L, -2, "current_priority");
        lua_pushinteger(L, (lua_Integer)tasks[i].uxBasePriority);
        lua_setfield(L, -2, "base_priority");
        lua_pushinteger(L, (lua_Integer)tasks[i].usStackHighWaterMark);
        lua_setfield(L, -2, "stack_high_water_mark_words");
        lua_pushinteger(L, (lua_Integer)tasks[i].usStackHighWaterMark * (lua_Integer)sizeof(StackType_t));
        lua_setfield(L, -2, "stack_high_water_mark_bytes");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    free(tasks);
    return 1;
#else
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    UBaseType_t words = uxTaskGetStackHighWaterMark(task);

    lua_newtable(L);
    lua_newtable(L);
    lua_pushstring(L, pcTaskGetName(task));
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, (lua_Integer)uxTaskPriorityGet(task));
    lua_setfield(L, -2, "current_priority");
    lua_pushinteger(L, (lua_Integer)uxTaskPriorityGet(task));
    lua_setfield(L, -2, "base_priority");
    lua_pushstring(L, "running");
    lua_setfield(L, -2, "state");
    lua_pushinteger(L, (lua_Integer)words);
    lua_setfield(L, -2, "stack_high_water_mark_words");
    lua_pushinteger(L, (lua_Integer)words * (lua_Integer)sizeof(StackType_t));
    lua_setfield(L, -2, "stack_high_water_mark_bytes");
    lua_rawseti(L, -2, 1);
    lua_pushstring(L, "configUSE_TRACE_FACILITY=0, only current task watermark is available");
    lua_setfield(L, -2, "_warning");
    return 1;
#endif
}

static int lua_module_system_heap_get_current_task(lua_State *L)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    UBaseType_t words = uxTaskGetStackHighWaterMark(task);

    lua_newtable(L);
    lua_pushstring(L, pcTaskGetName(task));
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, (lua_Integer)words);
    lua_setfield(L, -2, "stack_high_water_mark_words");
    lua_pushinteger(L, (lua_Integer)words * (lua_Integer)sizeof(StackType_t));
    lua_setfield(L, -2, "stack_high_water_mark_bytes");
    return 1;
}

static void lua_module_system_push_heap_table(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"get_info", lua_module_system_heap_get_info},
        {"get_task_watermarks", lua_module_system_heap_get_task_watermarks},
        {"get_current_task", lua_module_system_heap_get_current_task},
        {NULL, NULL},
    };

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_newtable(L);
    lua_module_system_push_heap_caps_constants(L);
    lua_setfield(L, -2, "caps");
}

static int lua_module_system_time(lua_State *L)
{
    time_t now;

    now = time(NULL);
    if (now < 0) {
        return luaL_error(L, "system clock not set");
    }

    lua_pushnumber(L, (lua_Number)now);
    return 1;
}

static int lua_module_system_date(lua_State *L)
{
    const char *fmt = luaL_optstring(L, 1, "%Y-%m-%d %H:%M:%S");
    char buf[128];
    struct tm local_time;
    time_t now;
    size_t len;

    now = time(NULL);
    localtime_r(&now, &local_time);

    len = strftime(buf, sizeof(buf), fmt, &local_time);
    if (len == 0) {
        return luaL_error(L, "system.date: format too long or produced empty string");
    }

    lua_pushstring(L, buf);
    return 1;
}

static int lua_module_system_millis(lua_State *L)
{
    int64_t us = esp_timer_get_time();

    lua_pushnumber(L, (lua_Number)(us / 1000));
    return 1;
}

static int lua_module_system_uptime(lua_State *L)
{
    int64_t us = esp_timer_get_time();

    lua_pushinteger(L, (lua_Integer)(us / 1000000));
    return 1;
}

static bool lua_module_system_get_sta_ip(char *buf, size_t buf_size)
{
    esp_netif_t *netif = NULL;
    esp_netif_ip_info_t ip_info = {0};

    if (!buf || buf_size == 0) {
        return false;
    }

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }

    snprintf(buf, buf_size, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

static int lua_module_system_ip(lua_State *L)
{
    char ip_buf[16];

    if (!lua_module_system_get_sta_ip(ip_buf, sizeof(ip_buf))) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, ip_buf);
    return 1;
}

static int lua_module_system_info(lua_State *L)
{
    char date_buf[32];
    struct tm local_time;
    time_t now;
    size_t psram_total;
    wifi_ap_record_t ap_info = {0};
    int64_t us;

    lua_newtable(L);

    us = esp_timer_get_time();
    lua_pushinteger(L, (lua_Integer)(us / 1000000));
    lua_setfield(L, -2, "uptime_s");

    now = time(NULL);
    lua_pushnumber(L, (lua_Number)now);
    lua_setfield(L, -2, "time");

    localtime_r(&now, &local_time);
    if (strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", &local_time) > 0) {
        lua_pushstring(L, date_buf);
        lua_setfield(L, -2, "date");
    }

    lua_pushinteger(L, (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    lua_setfield(L, -2, "sram_free");

    lua_pushinteger(L, (lua_Integer)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    lua_setfield(L, -2, "sram_total");

    lua_pushinteger(L, (lua_Integer)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    lua_setfield(L, -2, "sram_largest");

    psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total > 0) {
        lua_pushinteger(L, (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        lua_setfield(L, -2, "psram_free");

        lua_pushinteger(L, (lua_Integer)psram_total);
        lua_setfield(L, -2, "psram_total");
    }

    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        lua_pushinteger(L, (lua_Integer)ap_info.rssi);
        lua_setfield(L, -2, "wifi_rssi");

        if (ap_info.ssid[0] != '\0') {
            lua_pushstring(L, (const char *)ap_info.ssid);
            lua_setfield(L, -2, "wifi_ssid");
        }
    }

    return 1;
}

int luaopen_system(lua_State *L)
{
    lua_newtable(L);

    lua_pushcfunction(L, lua_module_system_time);
    lua_setfield(L, -2, "time");

    lua_pushcfunction(L, lua_module_system_date);
    lua_setfield(L, -2, "date");

    lua_pushcfunction(L, lua_module_system_millis);
    lua_setfield(L, -2, "millis");

    lua_pushcfunction(L, lua_module_system_uptime);
    lua_setfield(L, -2, "uptime");

    lua_pushcfunction(L, lua_module_system_ip);
    lua_setfield(L, -2, "ip");

    lua_pushcfunction(L, lua_module_system_info);
    lua_setfield(L, -2, "info");

    lua_module_system_push_heap_table(L);
    lua_setfield(L, -2, "heap");

    return 1;
}

esp_err_t lua_module_system_register(void)
{
    return cap_lua_register_module("system", luaopen_system);
}
