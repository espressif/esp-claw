/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_fs.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "ramfs.h"
#include "wear_levelling.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_board_manager_includes.h"

#define APP_FS_SYSTEM_PARTITION_LABEL   "system"
#define APP_FS_STORAGE_PARTITION_LABEL  "storage"
#define APP_FS_SDCARD_DEVICE_NAME       "fs_sdcard"

#define APP_FS_RAMFS_MAX_FILES          (8)
#define APP_FS_RAMFS_MAX_BYTES          (512 * 1024)

static const char *TAG = "app_fs";

static const char *const s_system_base_path = "/system";
static const char *const s_flash_storage_base_path = "/fatfs";
static const char *const s_ramfs_base_path = "/ramfs";
static const char *const s_recovery_dir_name = ".recovery";

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

// Active mount point of the writable storage: the flash fatfs partition by
// default, or the SD card's own mount point (taken from the device handle)
// when an SD card is in use. Exposed through app_fs_storage_base_path().
static char s_storage_base_path[32] = "/fatfs";

// Dot-prefixed by convention so cap_files list_dir hides the seed tree from the LLM.
static esp_err_t build_recovery_path(char *path, size_t path_size)
{
    int len = snprintf(path, path_size, "%s/%s", s_system_base_path, s_recovery_dir_name);
    ESP_RETURN_ON_FALSE(len > 0 && len < (int)path_size, ESP_ERR_INVALID_SIZE, TAG, "Recovery path too long");
    return ESP_OK;
}

static void log_fatfs_info(const char *base_path)
{
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(base_path, &total, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to query FATFS info for %s: %s", base_path, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "FATFS at %s total=%u used=%u",
                 base_path,
                 (unsigned int)total,
                 (unsigned int)(total - free_bytes));
    }
}

static esp_err_t copy_file(const char *src_path, const char *dst_path)
{
    esp_err_t ret = ESP_OK;
    char buf[512];
    size_t n;
    FILE *dst = NULL;
    FILE *src = fopen(src_path, "rb");
    ESP_RETURN_ON_FALSE(src, ESP_FAIL, TAG, "open src failed: %s (%s)", src_path, strerror(errno));

    dst = fopen(dst_path, "wb");
    ESP_GOTO_ON_FALSE(dst, ESP_FAIL, cleanup, TAG, "open dst failed: %s (%s)", dst_path, strerror(errno));

    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        ESP_GOTO_ON_FALSE(fwrite(buf, 1, n, dst) == n, ESP_FAIL, cleanup, TAG,
                          "write failed: %s (%s)", dst_path, strerror(errno));
    }

cleanup:
    if (dst) {
        fclose(dst);
    }
    fclose(src);
    return ret;
}

// Restore files from src_dir (e.g. /system/.recovery) into dst_dir (the fatfs
// partition), copying only entries that are missing in dst_dir. Existing files
// are left untouched regardless of whether their content is intact - this both
// re-seeds a freshly formatted partition and patches up partial data loss.
// Missing source dir is not an error; there is simply nothing to recover.
static esp_err_t recover_missing_files(const char *src_dir, const char *dst_dir)
{
    DIR *dir = opendir(src_dir);
    if (!dir) {
        ESP_LOGW(TAG, "recovery source unavailable: %s (%s)", src_dir, strerror(errno));
        return ESP_OK;
    }

    esp_err_t result = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char src_path[256];
        char dst_path[256];
        if (snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name) >= (int)sizeof(src_path) ||
            snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, entry->d_name) >= (int)sizeof(dst_path)) {
            ESP_LOGW(TAG, "path too long, skipping %s/%s", src_dir, entry->d_name);
            result = ESP_ERR_INVALID_SIZE;
            continue;
        }

        struct stat st;
        if (stat(src_path, &st) != 0) {
            ESP_LOGW(TAG, "stat failed: %s (%s)", src_path, strerror(errno));
            result = ESP_FAIL;
            continue;
        }

        struct stat dst_st;
        bool dst_exists = (stat(dst_path, &dst_st) == 0);

        if (S_ISDIR(st.st_mode)) {
            if (!dst_exists && mkdir(dst_path, 0777) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir failed: %s (%s)", dst_path, strerror(errno));
                result = ESP_FAIL;
                continue;
            }
            // Recurse so files missing inside an existing directory are restored too.
            esp_err_t sub_err = recover_missing_files(src_path, dst_path);
            if (sub_err != ESP_OK) {
                result = sub_err;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (dst_exists) {
                continue;  // keep whatever is already there, even if corrupt
            }
            esp_err_t copy_err = copy_file(src_path, dst_path);
            if (copy_err != ESP_OK) {
                result = copy_err;
            } else {
                ESP_LOGW(TAG, "recovered %s -> %s", src_path, dst_path);
            }
        }
    }

    closedir(dir);
    return result;
}

#if defined(CONFIG_ESP_BOARD_DEV_FS_FAT_SUPPORT)
// Return the mount point of the SD card that the board manager mounted, or NULL
// when there is no usable SD card. The mount point is taken from the device
// handle (board_devices.yaml decides where the card lives) rather than assumed.
// Hot-plug is out of scope, so the handle is sampled once at boot.
static const char *storage_sdcard_mount_point(void)
{
    dev_fs_fat_handle_t *handle = NULL;
    esp_err_t err = esp_board_device_get_handle(APP_FS_SDCARD_DEVICE_NAME, (void **)&handle);
    if (err != ESP_OK || handle == NULL) {
        ESP_LOGI(TAG, "No SD card device, using flash fatfs");
        return NULL;
    }

    if (!handle->mount_point || !handle->mount_point[0] || handle->card == NULL) {
        ESP_LOGW(TAG, "SD card device present but not mounted, using flash fatfs");
        return NULL;
    }

    return handle->mount_point;
}
#endif  /* CONFIG_ESP_BOARD_DEV_FS_FAT_SUPPORT */

static esp_err_t app_fs_init_system(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_ro(s_system_base_path,
                                                  APP_FS_SYSTEM_PARTITION_LABEL,
                                                  &mount_config);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to mount system FATFS: %s", esp_err_to_name(err));

    log_fatfs_info(s_system_base_path);
    return ESP_OK;
}

static esp_err_t app_fs_init_storage(void)
{
    char recovery_path[64];
    ESP_RETURN_ON_ERROR(build_recovery_path(recovery_path, sizeof(recovery_path)), TAG, "Failed to build recovery path");

#if defined(CONFIG_ESP_BOARD_DEV_FS_FAT_SUPPORT)
    // 1. Prefer an SD card that the board manager already mounted. The active
    //    storage path becomes the card's own mount point (from the handle), so
    //    the rest of the app follows it via app_fs_storage_base_path(). The
    //    flash fatfs partition is left unmounted in this case. Only available
    //    when the board manager was built with fs_fat (SD card) support.
    const char *sd_mount = storage_sdcard_mount_point();
    if (sd_mount) {
        strlcpy(s_storage_base_path, sd_mount, sizeof(s_storage_base_path));
        ESP_LOGI(TAG, "Using SD card at '%s' as fatfs storage", s_storage_base_path);
        esp_err_t rec = recover_missing_files(recovery_path, s_storage_base_path);
        if (rec != ESP_OK) {
            ESP_LOGW(TAG, "Recovery into SD card incomplete: %s", esp_err_to_name(rec));
        }
        log_fatfs_info(s_storage_base_path);
        return ESP_OK;
    }
#endif  /* CONFIG_ESP_BOARD_DEV_FS_FAT_SUPPORT */

    // 2. No usable SD card: mount the flash fatfs partition. If it is corrupt
    //    and cannot be mounted, format it and remount.
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    strlcpy(s_storage_base_path, s_flash_storage_base_path, sizeof(s_storage_base_path));
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(s_flash_storage_base_path,
                                                     APP_FS_STORAGE_PARTITION_LABEL,
                                                     &mount_config,
                                                     &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Flash fatfs mount failed: %s", esp_err_to_name(err));
        return err;
    }

    // 3. Restore any files present under /system/.recovery but missing from the
    //    fatfs partition (covers both a freshly formatted partition and partial
    //    data loss). Existing files are kept as-is.
    esp_err_t rec = recover_missing_files(recovery_path, s_storage_base_path);
    if (rec != ESP_OK) {
        ESP_LOGW(TAG, "Recovery into flash fatfs incomplete: %s", esp_err_to_name(rec));
    }

    log_fatfs_info(s_storage_base_path);
    return ESP_OK;
}

const char *app_fs_storage_base_path(void)
{
    return s_storage_base_path;
}

const char *app_fs_system_base_path(void)
{
    return s_system_base_path;
}

static esp_err_t app_fs_init_ramfs(void)
{
    ramfs_config_t config = {
        .base_path = s_ramfs_base_path,
        .max_files = APP_FS_RAMFS_MAX_FILES,
        .max_bytes = APP_FS_RAMFS_MAX_BYTES,
        .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    };
    ESP_RETURN_ON_ERROR(ramfs_register(&config), TAG,
                        "Failed to mount RAMFS at %s", s_ramfs_base_path);

    ESP_LOGI(TAG, "RAMFS mounted at %s max_files=%u max_bytes=%u",
             s_ramfs_base_path,
             (unsigned int)APP_FS_RAMFS_MAX_FILES,
             (unsigned int)APP_FS_RAMFS_MAX_BYTES);

    return ESP_OK;
}

esp_err_t app_fs_init(void)
{
    ESP_RETURN_ON_ERROR(app_fs_init_system(), TAG, "Failed to mount system FATFS");
    ESP_RETURN_ON_ERROR(app_fs_init_storage(), TAG, "Failed to mount storage FATFS");
#ifdef CONFIG_SPIRAM
    ESP_RETURN_ON_ERROR(app_fs_init_ramfs(), TAG, "Failed to mount RAMFS");
#endif
    return ESP_OK;
}
