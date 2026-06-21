/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "app_claw.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Resolved storage paths threaded through the capability framework
 *
 *         Built inside app_claw from the logical homes registered in claw_paths
 *         (see claw_paths.h); not part of the public app_claw API. Each field is
 *         an absolute path derived from a home plus a fixed subdirectory.
 */
typedef struct app_claw_storage_paths_t {
    char fatfs_base_path[APP_CLAW_PATH_LEN];          /**< Writable data root */
    char memory_session_root[APP_CLAW_PATH_LEN];      /**< Per-session conversation state */
    char memory_root_dir[APP_CLAW_PATH_LEN];          /**< Long-term memory store */
    char skills_root_dir[APP_CLAW_PATH_LEN];          /**< Writable skills root */
    char system_skills_root_dir[APP_CLAW_PATH_LEN];   /**< Read-only firmware-baked skills root */
    char lua_root_dir[APP_CLAW_PATH_LEN];             /**< Lua scripts root */
    char router_rules_path[APP_CLAW_FILE_PATH_LEN];   /**< Event router rules file */
    char scheduler_rules_path[APP_CLAW_FILE_PATH_LEN];/**< Scheduler rules file */
    char im_attachment_root[APP_CLAW_PATH_LEN];       /**< IM attachment inbox */
} app_claw_storage_paths_t;

#ifdef __cplusplus
}
#endif
