/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_platform.h"

#if CONFIG_APP_CLAW_CAP_IM_FEISHU
#include "cap_im_feishu.h"
#endif
#if CONFIG_APP_CLAW_CAP_IM_QQ
#include "cap_im_qq.h"
#endif
#if CONFIG_APP_CLAW_CAP_IM_TG
#include "cap_im_tg.h"
#endif
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#include "esp_check.h"

static const char *TAG = "cap_im_platform";

esp_err_t cap_im_platform_register_groups(void)
{
#if CONFIG_APP_CLAW_CAP_IM_FEISHU
    ESP_RETURN_ON_ERROR(cap_im_feishu_register_group(), TAG, "register Feishu IM group failed");
#endif
#if CONFIG_APP_CLAW_CAP_IM_QQ
    ESP_RETURN_ON_ERROR(cap_im_qq_register_group(), TAG, "register QQ IM group failed");
#endif
#if CONFIG_APP_CLAW_CAP_IM_TG
    ESP_RETURN_ON_ERROR(cap_im_tg_register_group(), TAG, "register Telegram IM group failed");
#endif
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
    ESP_RETURN_ON_ERROR(cap_im_wechat_register_group(), TAG, "register WeChat IM group failed");
#endif
    return ESP_OK;
}
