/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "claw_ramfs.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wifi_manager.h"
#include "wear_levelling.h"
#include "time.h"
#include "nvs_flash.h"
#include "http_server.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_board_manager_includes.h"
#include "captive_dns.h"
#include "cmd_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#include "app_config.h"
#include "app_message_inbox.h"
#include "app_status_screen.h"
#include "app_touch_xpt2046.h"
#include "app_boot_button.h"
#include "claw_event_publisher.h"
#include "sdcard_mount.h"
#include "settings_store.h"
#include "spi_bus_arbiter.h"
#include "app_sd_mirror.h"

#define APP_ENABLE_MEM_LOG        (0)

#define APP_FATFS_PARTITION_LABEL "storage"
#define APP_RAMFS_BASE_PATH       "/ramfs"
#define APP_RAMFS_MAX_FILES       (8)
#define APP_RAMFS_MAX_BYTES       (512 * 1024)

static const char *TAG = "app";

static app_config_t *s_config;
static app_claw_config_t *s_claw_config;
static app_claw_storage_paths_t *s_claw_paths;

static const char *app_fatfs_base_path = "/fatfs";

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

/* Bus-arbiter shims passed to settings_store so its SD-card mirror writes
 * never collide with the LCD on the shared SPI host. */
static bool sd_settings_bus_lock(void)
{
    return spi_bus_arbiter_lock(2000) == ESP_OK;
}

static void sd_settings_bus_unlock(bool was_locked)
{
    if (was_locked) {
        spi_bus_arbiter_unlock();
    }
}

static esp_err_t app_allocate_runtime_state(void)
{
    if (!s_config) {
        s_config = calloc(1, sizeof(*s_config));
    }
    if (!s_claw_config) {
        s_claw_config = calloc(1, sizeof(*s_claw_config));
    }
    if (!s_claw_paths) {
        s_claw_paths = calloc(1, sizeof(*s_claw_paths));
    }

    if (!s_config || !s_claw_config || !s_claw_paths) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void app_free_runtime_state(void)
{
    free(s_claw_paths);
    s_claw_paths = NULL;

    free(s_claw_config);
    s_claw_config = NULL;

    free(s_config);
    s_config = NULL;
}

static void on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    const char *ap_ssid = status.ap_active ? status.ap_ssid : NULL;
    const char *sta_ip = (connected && status.sta_ip) ? status.sta_ip : NULL;

    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s sta_ip=%s",
             connected,
             status.ap_active,
             status.mode ? status.mode : "off",
             ap_ssid ? ap_ssid : "(none)",
             sta_ip ? sta_ip : "(none)");

    esp_err_t err = app_claw_set_network_status(connected, ap_ssid, sta_ip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update network emote: %s", esp_err_to_name(err));
    }

    if (s_config) {
        app_status_screen_set_settings(s_config);
    }
    app_status_screen_request_refresh();
}

static void on_message_inbox_changed(void *user_ctx)
{
    (void)user_ctx;
    app_status_screen_request_refresh();
}

static void on_im_message_observed(const char *channel,
                                   const char *chat_id,
                                   const char *sender_id,
                                   const char *text,
                                   int64_t timestamp_ms,
                                   void *user_ctx)
{
    (void)chat_id;
    (void)user_ctx;
    app_message_inbox_record(channel, sender_id, text, timestamp_ms);
}

static void on_boot_button_pressed(void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "BOOT pressed -> next page");
    app_status_screen_next_page();
}

static void on_touch_tap(int x, int y, int pressure, void *user_ctx)
{
    (void)pressure;
    (void)user_ctx;
    ESP_LOGI(TAG, "Touch tap: x=%d y=%d", x, y);
}

static void on_touch_gesture(app_touch_gesture_t gesture,
                             int start_x, int start_y,
                             int end_x, int end_y,
                             void *user_ctx)
{
    (void)start_x; (void)start_y; (void)end_x; (void)end_y; (void)user_ctx;
    switch (gesture) {
    case APP_TOUCH_GESTURE_TAP:
        app_status_screen_next_page();
        break;
    case APP_TOUCH_GESTURE_SWIPE_UP:
        app_status_screen_scroll_messages(1);
        break;
    case APP_TOUCH_GESTURE_SWIPE_DOWN:
        app_status_screen_scroll_messages(-1);
        break;
    case APP_TOUCH_GESTURE_SWIPE_LEFT:
    case APP_TOUCH_GESTURE_SWIPE_RIGHT:
        app_status_screen_next_page();
        break;
    default:
        break;
    }
}

static esp_err_t app_claw_init_storage_paths(app_claw_storage_paths_t *paths)
{
    if (!paths || !app_fatfs_base_path || app_fatfs_base_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(paths, 0, sizeof(*paths));

    if (strlcpy(paths->fatfs_base_path, app_fatfs_base_path, sizeof(paths->fatfs_base_path)) >= sizeof(paths->fatfs_base_path) ||
        snprintf(paths->memory_session_root, sizeof(paths->memory_session_root), "%s/sessions", app_fatfs_base_path) >= sizeof(paths->memory_session_root) ||
        snprintf(paths->memory_root_dir, sizeof(paths->memory_root_dir), "%s/memory", app_fatfs_base_path) >= sizeof(paths->memory_root_dir) ||
        snprintf(paths->skills_root_dir, sizeof(paths->skills_root_dir), "%s/skills", app_fatfs_base_path) >= sizeof(paths->skills_root_dir) ||
        snprintf(paths->lua_root_dir, sizeof(paths->lua_root_dir), "%s/scripts", app_fatfs_base_path) >= sizeof(paths->lua_root_dir) ||
        snprintf(paths->router_rules_path, sizeof(paths->router_rules_path), "%s/router_rules/router_rules.json", app_fatfs_base_path) >= sizeof(paths->router_rules_path) ||
        snprintf(paths->scheduler_rules_path, sizeof(paths->scheduler_rules_path), "%s/scheduler/schedules.json", app_fatfs_base_path) >= sizeof(paths->scheduler_rules_path) ||
        snprintf(paths->im_attachment_root, sizeof(paths->im_attachment_root), "%s/inbox", app_fatfs_base_path) >= sizeof(paths->im_attachment_root)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t main_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static esp_err_t main_save_config(const app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = app_config_validate_wifi(config, NULL);
    if (err != ESP_OK) {
        return err;
    }

    return app_config_save(config);
}

static esp_err_t main_get_wifi_status(http_server_wifi_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    status->ap_ip = wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;
    return ESP_OK;
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t main_wechat_login_start(const char *account_id, bool force)
{
    return cap_im_wechat_qr_login_start(account_id, force);
}

static esp_err_t main_wechat_login_get_status(http_server_wechat_login_status_t *status)
{
    cap_im_wechat_qr_login_status_t *raw = NULL;
    esp_err_t err;

    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    raw = calloc(1, sizeof(*raw));
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_wechat_qr_login_get_status(raw);
    if (err != ESP_OK) {
        free(raw);
        return err;
    }

    memset(status, 0, sizeof(*status));
    status->active = raw->active;
    status->configured = raw->configured;
    status->completed = raw->completed;
    status->persisted = raw->persisted;
    strlcpy(status->session_key, raw->session_key, sizeof(status->session_key));
    strlcpy(status->status, raw->status, sizeof(status->status));
    strlcpy(status->message, raw->message, sizeof(status->message));
    strlcpy(status->qr_data_url, raw->qr_data_url, sizeof(status->qr_data_url));
    strlcpy(status->account_id, raw->account_id, sizeof(status->account_id));
    strlcpy(status->user_id, raw->user_id, sizeof(status->user_id));
    strlcpy(status->token, raw->token, sizeof(status->token));
    strlcpy(status->base_url, raw->base_url, sizeof(status->base_url));
    free(raw);
    return ESP_OK;
}

static esp_err_t main_wechat_login_cancel(void)
{
    return cap_im_wechat_qr_login_cancel();
}

static esp_err_t main_wechat_login_mark_persisted(void)
{
    return cap_im_wechat_qr_login_mark_persisted();
}
#endif

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
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t err;

    err = esp_vfs_fat_spiflash_mount_rw_wl(app_fatfs_base_path,
                                           APP_FATFS_PARTITION_LABEL,
                                           &mount_config,
                                           &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_vfs_fat_info(app_fatfs_base_path, &total, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to query FATFS info: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "FATFS mounted total=%u used=%u",
                 (unsigned int)total,
                 (unsigned int)(total - free_bytes));
    }

    return ESP_OK;
}

static esp_err_t init_ramfs(void)
{
    claw_ramfs_config_t config = {
        .base_path = APP_RAMFS_BASE_PATH,
        .max_files = APP_RAMFS_MAX_FILES,
        .max_bytes = APP_RAMFS_MAX_BYTES,
        .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    };
    esp_err_t err = claw_ramfs_register(&config);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount RAMFS at %s: %s", APP_RAMFS_BASE_PATH, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "RAMFS mounted at %s max_files=%u max_bytes=%u",
             APP_RAMFS_BASE_PATH,
             (unsigned int)APP_RAMFS_MAX_FILES,
             (unsigned int)APP_RAMFS_MAX_BYTES);

    return ESP_OK;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t err = ESP_OK;

    if (!timezone || timezone[0] == '\0') {
        ESP_LOGE(TAG, "Timezone is empty.");
        err = ESP_ERR_INVALID_ARG;
        goto tz_default;
    }

    if (setenv("TZ", timezone, 1) != 0) {
        ESP_LOGE(TAG, "Failed to set TZ env");
        err = ESP_FAIL;
        goto tz_default;
    }
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return err;
}

#if APP_ENABLE_MEM_LOG

static void print_task_stack_info(void)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t s_task_status_snapshot[24];
    UBaseType_t count = uxTaskGetSystemState(s_task_status_snapshot,
                                             sizeof(s_task_status_snapshot) / sizeof(s_task_status_snapshot[0]),
                                             NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG,
                 "Task %s  %u",
                 s_task_status_snapshot[i].pcTaskName,
                 s_task_status_snapshot[i].usStackHighWaterMark);
    }
#endif
}

/* Periodic task: print internal free, minimum free, and PSRAM free every 20s */
static void memory_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Memory: internal_free=%u bytes, internal_min_free=%u bytes, psram_free=%u bytes",
                 (unsigned)internal_free, (unsigned)internal_min, (unsigned)psram_free);
        print_task_stack_info();
    }
}

#endif

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("http_reuse", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_ERROR_CHECK(app_allocate_runtime_state());
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(app_config_load(s_config));
    app_config_to_claw(s_config, s_claw_config);
    init_timezone(app_config_get_timezone(s_config)); // no need to check error
    ESP_ERROR_CHECK(esp_board_manager_init());

    /* UI helpers (status screen, inbox, BOOT key, touch) MUST be brought up
     * before app_claw_ui_start(): emote_start() takes ownership of the panel
     * via display_arbiter, so the splash is rendered first while the screen
     * is still free, then emote takes over. */
    ESP_ERROR_CHECK(app_message_inbox_init());
    app_message_inbox_set_change_callback(on_message_inbox_changed, NULL);
    claw_event_router_set_message_observer(on_im_message_observed, NULL);

    esp_err_t splash_err = app_status_screen_init();
    if (splash_err == ESP_OK) {
        app_status_screen_set_settings(s_config);
        app_status_screen_show_splash(2000);
    } else if (splash_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "status screen init failed: %s", esp_err_to_name(splash_err));
    }

    esp_err_t boot_err = app_boot_button_init(28, on_boot_button_pressed, NULL);
    if (boot_err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT button init failed: %s", esp_err_to_name(boot_err));
    }

    {
        app_touch_xpt2046_config_t tcfg = {
            .host = SPI2_HOST,
            .cs_gpio = 1,
            .irq_gpio = -1,
            .screen_width = 320,
            .screen_height = 240,
            .calibration = {
                .raw_x_min = 200, .raw_x_max = 3428,
                .raw_y_min = 345, .raw_y_max = 3437,
                .swap_xy = true, .invert_x = true, .invert_y = true,
            },
        };
        esp_err_t touch_err = app_touch_xpt2046_init(&tcfg, on_touch_tap, NULL);
        if (touch_err != ESP_OK) {
            ESP_LOGW(TAG, "Touch init failed: %s", esp_err_to_name(touch_err));
        } else {
            app_touch_xpt2046_set_gesture_callback(on_touch_gesture, NULL);
        }
    }

    ESP_ERROR_CHECK(app_claw_ui_start());
    ESP_ERROR_CHECK(init_fatfs());
    ESP_ERROR_CHECK(init_ramfs());

    /* Persist incoming IM messages as JSONL into the inbox directory.
     * Best-effort — failures are logged but non-fatal. Prefer SD card if
     * we manage to mount one. */
    bool sd_available = (sdcard_mount_init() == ESP_OK);
    ESP_LOGI(TAG, "SD card status: %s%s%s",
             sd_available ? "mounted at " : "unavailable",
             sd_available ? sdcard_mount_get_mount_point() : "",
             sd_available ? "" : " (no card / no fs_sdcard device / mount failed)");

    /* Mirror the app_config NVS namespace to the SD card so that an
     * `idf.py erase-flash` (or any factory re-flash) does not wipe Wi-Fi /
     * LLM / IM credentials. After every successful settings_store_set_string
     * the entire namespace is dumped to <sd>/config/app_config.json. If NVS
     * is empty on boot but a backup file exists, we restore it and reload
     * the in-RAM config snapshot. */
    if (sd_available) {
        char backup_path[160];
        snprintf(backup_path, sizeof(backup_path), "%s/config/app_config.json",
                 sdcard_mount_get_mount_point());
        if (settings_store_set_backup_path(backup_path,
                                           sd_settings_bus_lock,
                                           sd_settings_bus_unlock) == ESP_OK) {
            bool nvs_empty = false;
            if (settings_store_is_namespace_empty(&nvs_empty) == ESP_OK
                    && nvs_empty) {
                esp_err_t rerr = settings_store_restore_from_backup();
                if (rerr == ESP_OK) {
                    ESP_LOGI(TAG, "Restored app_config from SD backup");
                    app_config_load(s_config);
                    app_config_to_claw(s_config, s_claw_config);
                    app_status_screen_set_settings(s_config);
                } else if (rerr != ESP_ERR_NOT_FOUND) {
                    ESP_LOGW(TAG, "SD restore failed: %s", esp_err_to_name(rerr));
                }
            } else {
                /* NVS already has values — make sure the SD mirror reflects
                 * them so a subsequent re-flash can restore. */
                (void)settings_store_dump_to_backup();
            }
        }

        /* Bidirectional sync of editable user-authored content (memory
         * persona files: soul / identity / user) between FATFS and SD.
         * Runs BEFORE app_claw_start() so claw_memory_init() picks up the
         * restored content on boot. Newer-mtime side wins; first boot
         * simply seeds the SD with the FATFS defaults. */
        {
            static const app_sd_mirror_entry_t mirror_entries[] = {
                { "/fatfs/memory/soul.md",     "memory/soul.md"     },
                { "/fatfs/memory/identity.md", "memory/identity.md" },
                { "/fatfs/memory/user.md",     "memory/user.md"     },
            };
            app_sd_mirror_config_t mirror_cfg = {
                .sd_root     = sdcard_mount_get_mount_point(),
                .entries     = mirror_entries,
                .entry_count = sizeof(mirror_entries) / sizeof(mirror_entries[0]),
                .bus_lock    = sd_settings_bus_lock,
                .bus_unlock  = sd_settings_bus_unlock,
            };
            (void)app_sd_mirror_sync(&mirror_cfg);

            /* Mirror the entire skills directory so user-created skills
             * (and the auto-updated skills_list.json) survive a re-flash.
             * Files deleted on one side are NOT propagated, so a fresh
             * factory image cannot wipe user skills on the SD card. */
            app_sd_mirror_set_bus_lock(sd_settings_bus_lock,
                                       sd_settings_bus_unlock);
            (void)app_sd_mirror_sync_dir(sdcard_mount_get_mount_point(),
                                         "/fatfs/skills",
                                         "skills");
        }
    }

    {
        char inbox_dir[160];
        const char *root = sd_available ? sdcard_mount_get_mount_point()
                                        : app_fatfs_base_path;
        snprintf(inbox_dir, sizeof(inbox_dir), "%s/inbox", root);
        app_message_inbox_set_persist_dir(inbox_dir);
        /* Restore the most recent persisted messages so the queue survives
         * reboots. 0 means "use the ring buffer's full capacity". */
        app_message_inbox_load_persisted(0);
    }

    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fatfs_base_path,
        .services = {
            .load_config = main_load_config,
            .save_config = main_save_config,
            .get_wifi_status = main_get_wifi_status,
            .restart_device = main_restart_device,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
            .wechat_login_start = main_wechat_login_start,
            .wechat_login_get_status = main_wechat_login_get_status,
            .wechat_login_cancel = main_wechat_login_cancel,
            .wechat_login_mark_persisted = main_wechat_login_mark_persisted,
#endif
        },
    }));
    /* Always register the /sdcard virtual mount, even if no card is currently
     * present. The alive_cb (sdcard_mount_alive_or_remount) hides the row
     * when no card is mounted and transparently mounts a freshly inserted
     * card on the next web refresh. */
    {
        const char *mp = sd_available ? sdcard_mount_get_mount_point() : "/sdcard";
        if (!mp) {
            mp = "/sdcard";
        }
        ESP_LOGI(TAG, "registering /sdcard -> %s as virtual mount (sd_available=%d)",
                 mp, sd_available);
        esp_err_t mount_err = http_server_register_mount("/sdcard", mp, "sd",
                                                         sdcard_mount_alive_or_remount);
        if (mount_err != ESP_OK) {
            ESP_LOGW(TAG, "register sdcard virtual mount failed: %s",
                     esp_err_to_name(mount_err));
        }
    }
    ESP_ERROR_CHECK(wifi_manager_register_state_callback(on_wifi_state_changed, NULL));

    esp_err_t wifi_err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = s_config->wifi_ssid,
        .sta_password = s_config->wifi_password,
        .ap_ssid = s_config->ap_ssid[0] ? s_config->ap_ssid : NULL,
        .ap_password = s_config->ap_password[0] ? s_config->ap_password : NULL,
        .ap_behavior = s_config->ap_behavior,
    });
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        ESP_ERROR_CHECK(http_server_start());
        if (captive_dns_start(&(captive_dns_config_t) {
                .ap_netif = wifi_manager_get_ap_netif(),
                .configure_dhcp_dns = true,
            }) != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
        }

        if (s_config->wifi_ssid[0] != '\0') {
            if (wifi_manager_wait_connected(30000) == ESP_OK) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGI(TAG, "Wi-Fi STA ready: %s", status.sta_ip);
            } else {
                ESP_LOGW(TAG, "STA could not connect, dropped to AP fallback");
            }
        }

        wifi_manager_status_t status = {0};
        wifi_manager_get_status(&status);
        if (status.ap_active) {
            const char *portal_auth = s_config->ap_password[0] ? "wpa2" : "open";
            ESP_LOGW(TAG,
                     "*** Provisioning portal: SSID=\"%s\" (auth=%s) IP=%s URL=http://%s/ ***",
                     status.ap_ssid,
                     portal_auth,
                     status.ap_ip,
                     status.ap_ip);
        }
    }

    ESP_ERROR_CHECK(app_claw_init_storage_paths(s_claw_paths));
    ESP_ERROR_CHECK(app_claw_start(s_claw_config, s_claw_paths));
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    ESP_ERROR_CHECK(http_server_webim_bind_im());
#endif

    register_wifi_command();

#if APP_ENABLE_MEM_LOG
    /* Start memory monitor: print internal free, min free, PSRAM free every 20s */
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif

    app_free_runtime_state();
}
