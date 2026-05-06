set(app_claw_requires
    claw_cap
    claw_core
    claw_event_router
    claw_memory
    claw_skill
    freertos
)

set(app_claw_srcs
    "app_claw.c"
    "app_capabilities.c"
    "app_lua_modules.c"
)

if(CONFIG_APP_CLAW_ENABLE_CLI)
    list(APPEND app_claw_srcs "app_claw_cli.c")
    list(APPEND app_claw_requires console json)
endif()

if(CONFIG_APP_CLAW_ENABLE_EMOTE)
    list(APPEND app_claw_requires emote)
endif()

if(CONFIG_APP_CLAW_CAP_FILES)
    list(APPEND app_claw_requires cap_files)
endif()

if(CONFIG_APP_CLAW_CAP_IM_FEISHU)
    list(APPEND app_claw_requires cap_im_feishu)
endif()

if(CONFIG_APP_CLAW_CAP_IM_QQ)
    list(APPEND app_claw_requires cap_im_qq)
endif()

if(CONFIG_APP_CLAW_CAP_IM_TG)
    list(APPEND app_claw_requires cap_im_tg)
endif()

if(CONFIG_APP_CLAW_CAP_IM_WECHAT)
    list(APPEND app_claw_requires cap_im_wechat)
endif()

if(CONFIG_APP_CLAW_CAP_IM_LOCAL)
    list(APPEND app_claw_requires cap_im_local)
endif()

if(CONFIG_APP_CLAW_CAP_LLM_INSPECT)
    list(APPEND app_claw_requires cap_llm_inspect)
endif()

if(CONFIG_APP_CLAW_CAP_LUA)
    list(APPEND app_claw_requires cap_lua)
endif()

if(CONFIG_APP_CLAW_CAP_MCP_CLIENT)
    list(APPEND app_claw_requires cap_mcp_client)
endif()

if(CONFIG_APP_CLAW_CAP_MCP_SERVER)
    list(APPEND app_claw_requires cap_mcp_server)
endif()

if(CONFIG_APP_CLAW_CAP_ROUTER_MGR)
    list(APPEND app_claw_requires cap_router_mgr)
endif()

if(CONFIG_APP_CLAW_CAP_SCHEDULER)
    list(APPEND app_claw_requires cap_scheduler)
endif()

if(CONFIG_APP_CLAW_CAP_SESSION_MGR)
    list(APPEND app_claw_requires cap_session_mgr)
endif()

if(CONFIG_APP_CLAW_CAP_SKILL_MGR)
    list(APPEND app_claw_requires cap_skill_mgr)
endif()

if(CONFIG_APP_CLAW_CAP_SYSTEM)
    list(APPEND app_claw_requires cap_system)
endif()

if(CONFIG_APP_CLAW_CAP_TIME)
    list(APPEND app_claw_requires cap_time)
endif()

if(CONFIG_APP_CLAW_CAP_WEB_SEARCH)
    list(APPEND app_claw_requires cap_web_search)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_BOARD_MANAGER)
    list(APPEND app_claw_requires lua_module_board_manager)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_IMU)
    list(APPEND app_claw_requires lua_module_imu)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_MAGNETOMETER)
    list(APPEND app_claw_requires lua_module_magnetometer)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_ENVIRONMENTAL_SENSOR)
    list(APPEND app_claw_requires lua_module_environmental_sensor)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_FUEL_GAUGE)
    list(APPEND app_claw_requires lua_module_fuel_gauge)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_TOUCH)
    list(APPEND app_claw_requires lua_module_touch)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_IR)
    list(APPEND app_claw_requires lua_module_ir)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_CAPABILITY)
    list(APPEND app_claw_requires lua_module_call_capability)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_BUTTON)
    list(APPEND app_claw_requires lua_module_button)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_KNOB)
    list(APPEND app_claw_requires lua_module_knob)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_ADC)
    list(APPEND app_claw_requires lua_module_adc)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_DELAY)
    list(APPEND app_claw_requires lua_module_delay)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_DISPLAY)
    list(APPEND app_claw_requires lua_module_display)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_LCD)
    list(APPEND app_claw_requires lua_module_lcd)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_ESP_HEAP)
    list(APPEND app_claw_requires lua_module_esp_heap)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_EVENT_PUBLISHER)
    list(APPEND app_claw_requires lua_module_event_publisher)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_GPIO)
    list(APPEND app_claw_requires lua_module_gpio)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_I2C)
    list(APPEND app_claw_requires lua_module_i2c)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_SSD1306)
    list(APPEND app_claw_requires lua_module_ssd1306)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_LED_STRIP)
    list(APPEND app_claw_requires lua_module_led_strip)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_MCPWM)
    list(APPEND app_claw_requires lua_module_mcpwm)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_STORAGE)
    list(APPEND app_claw_requires lua_module_storage)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_SYSTEM)
    list(APPEND app_claw_requires lua_module_system)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_UART)
    list(APPEND app_claw_requires lua_module_uart)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_HTTP)
    list(APPEND app_claw_requires lua_module_http)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_AUDIO AND CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT)
    list(APPEND app_claw_requires lua_module_audio)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_LCD_TOUCH AND CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT)
    list(APPEND app_claw_requires lua_module_lcd_touch)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_CAMERA AND CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
    list(APPEND app_claw_requires lua_module_camera)
endif()

idf_component_register(
    SRCS
        ${app_claw_srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        ${app_claw_requires}
)
