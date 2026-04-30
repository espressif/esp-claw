set(main_requires
    app_claw
    app_config
    captive_dns
    http_server
    wifi_manager
    cmd_wifi
    cap_voice
)

if(CONFIG_APP_CLAW_CAP_IM_WECHAT)
    list(APPEND main_requires cap_im_wechat)
endif()

idf_component_register(
    SRCS
        "main.c"
    REQUIRES
        ${main_requires}
)
