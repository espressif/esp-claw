/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_ota.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "adf_event_hub.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_service.h"
#include "esp_system.h"
#include "esp_ota_service.h"
#include "esp_ota_service_checker_app.h"
#include "esp_ota_service_checker_manifest.h"
#include "esp_ota_service_source_fs.h"
#include "esp_ota_service_source_http.h"
#include "esp_ota_service_target_app.h"
#include "esp_ota_service_verifier_sha256.h"

#include "sdkconfig.h"

static const char APP_OTA_TAG[] = "app_ota";

#define APP_OTA_EVT_DONE_BIT  ((EventBits_t)BIT0)
#define APP_OTA_EVT_FAIL_BIT  ((EventBits_t)BIT1)
#define APP_OTA_EVT_SKIP_BIT  ((EventBits_t)BIT2)

#define APP_OTA_SESSION_WAIT_MS  (600000U)

#if CONFIG_APP_OTA_HTTP_ENABLE || CONFIG_APP_OTA_FS_ENABLE

typedef struct {
    EventGroupHandle_t eg;
    char               channel[6];
} app_ota_evt_ctx_t;

static void app_ota_service_on_adf_event(const adf_event_t *event, void *handler_ctx)
{
    app_ota_evt_ctx_t *evc = (app_ota_evt_ctx_t *)handler_ctx;
    if (event == NULL || event->payload == NULL || event->payload_len < sizeof(esp_ota_service_event_t) || evc == NULL ||
        evc->eg == NULL) {
        return;
    }
    const esp_ota_service_event_t *evt = (const esp_ota_service_event_t *)event->payload;

    switch (evt->id) {
        case ESP_OTA_SERVICE_EVT_SESSION_BEGIN:
            ESP_LOGI(APP_OTA_TAG, "[%s] session started", evc->channel);
            break;
        case ESP_OTA_SERVICE_EVT_ITEM_VER_CHECK:
            if (evt->error == ESP_OK && !evt->ver_check.upgrade_available) {
                ESP_LOGW(APP_OTA_TAG, "[%s] version check: no upgrade (skip)", evc->channel);
                xEventGroupSetBits(evc->eg, APP_OTA_EVT_SKIP_BIT);
            } else {
                ESP_LOGI(APP_OTA_TAG, "[%s] version check err=%s upgrade=%d size=%" PRIu32, evc->channel,
                         esp_err_to_name(evt->error), evt->ver_check.upgrade_available ? 1 : 0, evt->ver_check.image_size);
            }
            break;
        case ESP_OTA_SERVICE_EVT_ITEM_BEGIN:
            ESP_LOGI(APP_OTA_TAG, "[%s] transfer started", evc->channel);
            break;
        case ESP_OTA_SERVICE_EVT_ITEM_PROGRESS:
            ESP_LOGD(APP_OTA_TAG, "[%s] progress %" PRIu32 "/%" PRIu32, evc->channel, evt->progress.bytes_written,
                     evt->progress.total_bytes);
            break;
        case ESP_OTA_SERVICE_EVT_ITEM_END:
            if (evt->item_end.status == ESP_OTA_SERVICE_ITEM_STATUS_OK) {
                ESP_LOGI(APP_OTA_TAG, "[%s] item finished OK", evc->channel);
            } else if (evt->item_end.status == ESP_OTA_SERVICE_ITEM_STATUS_SKIPPED) {
                ESP_LOGW(APP_OTA_TAG, "[%s] item skipped reason=%d", evc->channel, (int)evt->item_end.reason);
                xEventGroupSetBits(evc->eg, APP_OTA_EVT_SKIP_BIT);
            } else {
                ESP_LOGE(APP_OTA_TAG, "[%s] item failed %s reason=%d", evc->channel, esp_err_to_name(evt->error),
                         (int)evt->item_end.reason);
                xEventGroupSetBits(evc->eg, APP_OTA_EVT_FAIL_BIT);
            }
            break;
        case ESP_OTA_SERVICE_EVT_SESSION_END:
            if (evt->session_end.aborted || evt->session_end.failed_count > 0) {
                xEventGroupSetBits(evc->eg, APP_OTA_EVT_DONE_BIT | APP_OTA_EVT_FAIL_BIT);
            } else {
                xEventGroupSetBits(evc->eg, APP_OTA_EVT_DONE_BIT);
            }
            break;
        case ESP_OTA_SERVICE_EVT_MAX:
            break;
        default:
            break;
    }
}

static esp_err_t app_ota_attach_event_observer(esp_ota_service_t *svc, EventGroupHandle_t eg, app_ota_evt_ctx_t *ctx,
                                               const char *channel)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->eg = eg;
    strlcpy(ctx->channel, channel, sizeof(ctx->channel));

    adf_event_subscribe_info_t sub = ADF_EVENT_SUBSCRIBE_INFO_DEFAULT();
    sub.handler = app_ota_service_on_adf_event;
    sub.handler_ctx = ctx;
    return esp_service_event_subscribe((esp_service_t *)svc, &sub);
}

static esp_err_t app_ota_session_start_wait_and_finish(esp_ota_service_t *svc, EventGroupHandle_t eg, const char *channel,
                                                       const char *resource_for_log)
{
    ESP_LOGI(APP_OTA_TAG, "[%s] starting OTA resource=%s", channel, resource_for_log);

    esp_err_t ret = esp_service_start((esp_service_t *)svc);
    if (ret != ESP_OK) {
        ESP_LOGE(APP_OTA_TAG, "[%s] esp_service_start failed: %s", channel, esp_err_to_name(ret));
        esp_ota_service_destroy(svc);
        vEventGroupDelete(eg);
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(eg, APP_OTA_EVT_DONE_BIT | APP_OTA_EVT_FAIL_BIT | APP_OTA_EVT_SKIP_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(APP_OTA_SESSION_WAIT_MS));

    esp_err_t out = ESP_OK;
    if (bits & APP_OTA_EVT_SKIP_BIT) {
        out = ESP_OK;
    } else if ((bits & APP_OTA_EVT_DONE_BIT) && !(bits & APP_OTA_EVT_FAIL_BIT)) {
        esp_ota_service_destroy(svc);
        vEventGroupDelete(eg);
        ESP_LOGW(APP_OTA_TAG, "[%s] OTA finished — rebooting", channel);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (bits & APP_OTA_EVT_FAIL_BIT) {
        out = ESP_FAIL;
        ESP_LOGE(APP_OTA_TAG, "[%s] session reported failure", channel);
    } else {
        out = ESP_ERR_TIMEOUT;
        ESP_LOGE(APP_OTA_TAG, "[%s] session timed out", channel);
    }

    esp_ota_service_destroy(svc);
    vEventGroupDelete(eg);
    return out;
}

#endif /* CONFIG_APP_OTA_HTTP_ENABLE || CONFIG_APP_OTA_FS_ENABLE */

static void app_ota_handle_pending_verify(bool sta_station_connected_to_ap)
{
#if CONFIG_OTA_ENABLE_ROLLBACK
    bool pending = false;
    esp_err_t err = esp_ota_service_is_pending_verify(&pending);
    if (err != ESP_OK) {
        ESP_LOGW(APP_OTA_TAG, "pending verify probe failed: %s", esp_err_to_name(err));
        return;
    }
    if (!pending) {
        return;
    }

    if (sta_station_connected_to_ap) {
        ESP_LOGI(APP_OTA_TAG, "pending verify — confirming (STA online)");
        err = esp_ota_service_confirm_update();
        if (err != ESP_OK) {
            ESP_LOGE(APP_OTA_TAG, "confirm_update failed: %s", esp_err_to_name(err));
            (void)esp_ota_service_rollback();
        }
    } else {
        ESP_LOGW(APP_OTA_TAG, "pending verify but STA offline — rolling back");
        (void)esp_ota_service_rollback();
    }
#else
    (void)sta_station_connected_to_ap;
#endif
}

#if CONFIG_APP_OTA_HTTP_ENABLE

static bool app_ota_manifest_and_firmware_urls_ok(void)
{
    return CONFIG_APP_OTA_HTTP_MANIFEST_URL[0] != '\0' && CONFIG_APP_OTA_HTTP_FIRMWARE_URL[0] != '\0';
}

static esp_err_t app_ota_run_http_manifest_session_once(void)
{
    esp_err_t ret;
    esp_ota_service_cfg_t svc_cfg = ESP_OTA_SERVICE_CFG_DEFAULT();
    esp_ota_service_t *svc = NULL;
    ret = esp_ota_service_create(&svc_cfg, &svc);
    if (ret != ESP_OK || svc == NULL) {
        ESP_LOGE(APP_OTA_TAG, "[http] esp_ota_service_create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    EventGroupHandle_t ota_evt = xEventGroupCreate();
    if (ota_evt == NULL) {
        esp_ota_service_destroy(svc);
        return ESP_ERR_NO_MEM;
    }

    app_ota_evt_ctx_t ev_ctx;
    ret = app_ota_attach_event_observer(svc, ota_evt, &ev_ctx, "http");
    if (ret != ESP_OK) {
        ESP_LOGE(APP_OTA_TAG, "[http] subscribe failed: %s", esp_err_to_name(ret));
        vEventGroupDelete(ota_evt);
        esp_ota_service_destroy(svc);
        return ret;
    }

    esp_ota_service_source_http_cfg_t hcfg = {
        .timeout_ms = CONFIG_APP_OTA_HTTP_CHUNK_TIMEOUT_MS,
        .buf_size = 4096,
    };

    const esp_app_desc_t *app_desc = esp_app_get_description();
    esp_ota_service_checker_manifest_cfg_t mcfg = {
        .manifest_uri = CONFIG_APP_OTA_HTTP_MANIFEST_URL,
        .current_version = app_desc->version,
        .require_higher_version = true,
        .check_project_name = false,
    };

    esp_ota_service_checker_t *manifest_chk = esp_ota_service_checker_manifest_create(&mcfg);
    if (manifest_chk == NULL) {
        ESP_LOGE(APP_OTA_TAG, "[http] manifest checker create failed");
        vEventGroupDelete(ota_evt);
        esp_ota_service_destroy(svc);
        return ESP_ERR_NO_MEM;
    }

    esp_ota_service_check_result_t chk_result;
    memset(&chk_result, 0, sizeof(chk_result));
    esp_err_t cr = manifest_chk->check(manifest_chk, NULL, NULL, &chk_result);
    manifest_chk->destroy(manifest_chk);
    if (cr != ESP_OK) {
        ESP_LOGE(APP_OTA_TAG, "[http] manifest check failed: %s", esp_err_to_name(cr));
        vEventGroupDelete(ota_evt);
        esp_ota_service_destroy(svc);
        return cr;
    }
    if (!chk_result.upgrade_available) {
        ESP_LOGI(APP_OTA_TAG, "[http] running '%s' up to date (server '%s')", app_desc->version, chk_result.version);
        vEventGroupDelete(ota_evt);
        esp_ota_service_destroy(svc);
        return ESP_OK;
    }

    ESP_LOGI(APP_OTA_TAG, "[http] upgrade available '%s' -> '%s'", app_desc->version, chk_result.version);

    esp_ota_service_verifier_t *ver = NULL;
    if (chk_result.has_hash) {
        esp_ota_service_verifier_sha256_cfg_t vcfg;
        memcpy(vcfg.expected_hash, chk_result.hash, sizeof(vcfg.expected_hash));
        esp_err_t vr = esp_ota_service_verifier_sha256_create(&vcfg, &ver);
        if (vr != ESP_OK) {
            ESP_LOGW(APP_OTA_TAG, "[http] SHA256 verifier create failed (%s) — proceeding without integrity check",
                     esp_err_to_name(vr));
        }
    } else {
        ESP_LOGW(APP_OTA_TAG, "[http] manifest has no sha256 — integrity skipped");
    }

    esp_ota_service_source_t *src = NULL;
    esp_ota_service_target_t *tgt = NULL;
    ret = esp_ota_service_source_http_create(&hcfg, &src);
    if (ret == ESP_OK) {
        esp_ota_service_target_app_cfg_t app_cfg = {.bulk_flash_erase = true};
        ret = esp_ota_service_target_app_create(&app_cfg, &tgt);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(APP_OTA_TAG, "[http] source/target create failed: %s", esp_err_to_name(ret));
        if (src != NULL && src->destroy != NULL) {
            src->destroy(src);
        }
        if (tgt != NULL && tgt->destroy != NULL) {
            tgt->destroy(tgt);
        }
        if (ver != NULL && ver->destroy != NULL) {
            ver->destroy(ver);
        }
        vEventGroupDelete(ota_evt);
        esp_ota_service_destroy(svc);
        return ret;
    }

    esp_ota_upgrade_item_t items[] = {{
        .uri = CONFIG_APP_OTA_HTTP_FIRMWARE_URL,
        .partition_label = NULL,
        .source = src,
        .target = tgt,
        .verifier = ver,
        .checker = NULL,
        .skip_on_fail = false,
        .resumable = CONFIG_OTA_ENABLE_RESUME ? true : false,
    }};

    ret = esp_ota_service_set_upgrade_list(svc, items, sizeof(items) / sizeof(items[0]));
    if (ret != ESP_OK) {
        ESP_LOGE(APP_OTA_TAG, "[http] esp_ota_service_set_upgrade_list failed: %s", esp_err_to_name(ret));
        esp_ota_service_destroy(svc);
        vEventGroupDelete(ota_evt);
        return ret;
    }

    return app_ota_session_start_wait_and_finish(svc, ota_evt, "http", CONFIG_APP_OTA_HTTP_FIRMWARE_URL);
}

#endif /* CONFIG_APP_OTA_HTTP_ENABLE */

#if CONFIG_APP_OTA_FS_ENABLE

static esp_err_t app_ota_run_fs_upgrade_once(const char *abs_path_vfs)
{
    esp_ota_service_cfg_t svc_cfg = ESP_OTA_SERVICE_CFG_DEFAULT();
    esp_ota_service_t *svc = NULL;
    esp_err_t ret = esp_ota_service_create(&svc_cfg, &svc);
    if (ret != ESP_OK || svc == NULL) {
        ESP_LOGE(APP_OTA_TAG, "[fs] esp_ota_service_create failed %s", esp_err_to_name(ret));
        return ret;
    }

    EventGroupHandle_t eg = xEventGroupCreate();
    if (eg == NULL) {
        esp_ota_service_destroy(svc);
        return ESP_ERR_NO_MEM;
    }

    app_ota_evt_ctx_t ev_ctx;
    ret = app_ota_attach_event_observer(svc, eg, &ev_ctx, "fs");
    if (ret != ESP_OK) {
        ESP_LOGE(APP_OTA_TAG, "[fs] subscribe failed %s", esp_err_to_name(ret));
        vEventGroupDelete(eg);
        esp_ota_service_destroy(svc);
        return ret;
    }

    esp_ota_service_checker_app_cfg_t checker_cfg = {
        .version_policy =
            {
                .require_higher_version = CONFIG_APP_OTA_FS_REQUIRE_NEWER_SEMVER ? true : false,
                .check_project_name = CONFIG_APP_OTA_FS_CHECK_PROJECT_NAME ? true : false,
            },
        .should_upgrade = NULL,
        .should_upgrade_ctx = NULL,
    };

    esp_ota_service_checker_t *checker = esp_ota_service_checker_app_create(&checker_cfg);
    if (checker == NULL) {
        vEventGroupDelete(eg);
        esp_ota_service_destroy(svc);
        return ESP_ERR_NO_MEM;
    }

    esp_ota_service_source_fs_cfg_t sf_cfg = {
        .buf_size = CONFIG_APP_OTA_FS_READ_BUF_BYTES,
    };
    esp_ota_service_source_t *src = NULL;
    esp_ota_service_target_t *tgt = NULL;
    ret = esp_ota_service_source_fs_create(&sf_cfg, &src);
    if (ret == ESP_OK) {
        esp_ota_service_target_app_cfg_t tgt_cfg = {.bulk_flash_erase = true};
        ret = esp_ota_service_target_app_create(&tgt_cfg, &tgt);
    }
    if (ret != ESP_OK || src == NULL || tgt == NULL) {
        ESP_LOGE(APP_OTA_TAG, "[fs] source/target alloc failed %s", esp_err_to_name(ret));
        if (checker->destroy != NULL) {
            checker->destroy(checker);
        }
        if (src != NULL && src->destroy != NULL) {
            src->destroy(src);
        }
        if (tgt != NULL && tgt->destroy != NULL) {
            tgt->destroy(tgt);
        }
        vEventGroupDelete(eg);
        esp_ota_service_destroy(svc);
        return ret != ESP_OK ? ret : ESP_ERR_NO_MEM;
    }

    esp_ota_upgrade_item_t items[] = {{
        .uri = abs_path_vfs,
        .partition_label = NULL,
        .source = src,
        .target = tgt,
        .verifier = NULL,
        .checker = checker,
        .skip_on_fail = false,
        .resumable = CONFIG_OTA_ENABLE_RESUME ? true : false,
    }};

    ret = esp_ota_service_set_upgrade_list(svc, items, sizeof(items) / sizeof(items[0]));
    if (ret != ESP_OK) {
        ESP_LOGE(APP_OTA_TAG, "[fs] set_upgrade_list failed %s", esp_err_to_name(ret));
        esp_ota_service_destroy(svc);
        vEventGroupDelete(eg);
        return ret;
    }

    return app_ota_session_start_wait_and_finish(svc, eg, "fs", abs_path_vfs);
}

static esp_err_t app_ota_fs_boot_flow_at_impl(const char *path)
{
#if CONFIG_APP_OTA_FS_RUN_AT_BOOT
    if (path == NULL || path[0] != '/') {
        ESP_LOGW(APP_OTA_TAG, "[fs] invalid firmware path (need absolute VFS path)");
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st = {0};
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGD(APP_OTA_TAG, "[fs] no candidate at %s", path);
        return ESP_OK;
    }

    ESP_LOGI(APP_OTA_TAG, "[fs] boot_flow firmware stat ok size=%ld path=%s", (long)st.st_size, path);
    esp_err_t err = app_ota_run_fs_upgrade_once(path);
    if (err != ESP_OK) {
        ESP_LOGW(APP_OTA_TAG, "[fs] OTA aborted: %s", esp_err_to_name(err));
    }
    return err;
#else
    (void)path;
    return ESP_OK;
#endif
}

#endif /* CONFIG_APP_OTA_FS_ENABLE */

esp_err_t app_ota_fs_boot_flow_at(const char *firmware_abs_path)
{
#if CONFIG_APP_OTA_FS_ENABLE
    return app_ota_fs_boot_flow_at_impl(firmware_abs_path);
#else
    (void)firmware_abs_path;
    return ESP_OK;
#endif
}

esp_err_t app_ota_fs_boot_flow(void)
{
#if CONFIG_APP_OTA_FS_ENABLE && CONFIG_APP_OTA_FS_RUN_AT_BOOT
    return app_ota_fs_boot_flow_at_impl(CONFIG_APP_OTA_FS_FIRMWARE_ABS_PATH);
#else
    return ESP_OK;
#endif
}

esp_err_t app_ota_http_boot_flow(bool sta_station_connected_to_ap)
{
    ESP_LOGI(APP_OTA_TAG, "[http] boot_flow entry sta_online=%d", sta_station_connected_to_ap ? 1 : 0);
    app_ota_handle_pending_verify(sta_station_connected_to_ap);

#if CONFIG_APP_OTA_HTTP_ENABLE && CONFIG_APP_OTA_HTTP_RUN_AT_BOOT
    if (!sta_station_connected_to_ap || !app_ota_manifest_and_firmware_urls_ok()) {
        return ESP_OK;
    }

    ESP_LOGI(APP_OTA_TAG, "[http] manifest + firmware URLs configured — starting bootstrap");

    esp_err_t err = app_ota_run_http_manifest_session_once();
    if (err != ESP_OK) {
        ESP_LOGW(APP_OTA_TAG, "[http] bootstrap finished with errors: %s", esp_err_to_name(err));
    }
    return err;

#elif CONFIG_APP_OTA_HTTP_ENABLE
    (void)sta_station_connected_to_ap;
    return ESP_OK;
#else
    (void)sta_station_connected_to_ap;
    return ESP_OK;
#endif
}
