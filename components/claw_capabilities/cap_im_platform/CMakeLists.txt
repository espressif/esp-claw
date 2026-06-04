set(srcs
    "src/cap_im_attachment.c"
    "src/cap_im_platform.c"
)

if(CONFIG_APP_CLAW_CAP_IM_FEISHU)
    list(APPEND srcs
        "src/cap_im_feishu.c"
        "src/cmd_cap_im_feishu.c"
    )
endif()

if(CONFIG_APP_CLAW_CAP_IM_QQ)
    list(APPEND srcs
        "src/cap_im_qq.c"
        "src/cmd_cap_im_qq.c"
    )
endif()

if(CONFIG_APP_CLAW_CAP_IM_TG)
    list(APPEND srcs
        "src/cap_im_tg.c"
        "src/cmd_cap_im_tg.c"
    )
endif()

if(CONFIG_APP_CLAW_CAP_IM_WECHAT)
    list(APPEND srcs
        "src/cap_im_wechat.c"
        "src/cmd_cap_im_wechat.c"
    )
endif()

idf_component_register(
    SRCS
        ${srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        claw_cap
        claw_event_router
        esp_http_client
        http_reuse
        esp_event
        esp_http_client
        esp_timer
        esp_websocket_client
        freertos
        mbedtls
        console
)
