# Wave Rover MCP — Reference Sources

| Source | URL | Type | What was found | Relevant files/sections | Confidence |
|---|---|---|---|---|---|
| ESP-Claw repo | https://github.com/espressif/esp-claw | GitHub | rover_demo app pattern, cap_rover I2C driver, cap_mcp_server cap, PlatformIO configs in rover_demo/rover_s3 | application/rover_demo/, components/claw_capabilities/cap_rover/ | High |
| ESP-Claw tutorial | https://esp-claw.com/en/tutorial/ | Docs | Architecture, MCP server capability, capability registration | Tutorial pages | Medium (403 at plan time) |
| WAVE ROVER product | https://www.waveshare.com/wave-rover.htm | Product | SKU 25377, ESP32 board, 4WD N20 motors, INA219 UPS, OLED, 9-axis IMU, TF slot | Product page | High (403, confirmed via wiki) |
| WAVE ROVER English wiki | https://www.waveshare.com/wiki/WAVE_ROVER | Wiki | HTTP/serial/USB/ESP-NOW transports, JSON commands, Wi-Fi AP default, OLED boot | Wiki page | High (403, confirmed via ugv_base) |
| WAVE ROVER Chinese wiki | https://www.waveshare.net/wiki/WAVE_ROVER | Wiki | Returned 403; content inferred from ugv_base_general source | — | Medium |
| General Driver for Robots | https://www.waveshare.com/wiki/General_Driver_for_Robots | Wiki | ESP32-WROOM-32 MCU, motor GPIO, I2C pins, INA219 addr=0x42, OLED SSD1306 addr=0x3C, IMU QMI8658+AK09918 | ugv_config.h, oled_ctrl.h, battery_ctrl.h, IMU.h | High (via GitHub source) |
| ugv_base_general | https://github.com/waveshareteam/ugv_base_general | GitHub | Motor GPIO map, INA219, OLED, QMI8658+AK09918, JSON command protocol, heartbeat | General_Driver/*.h | High |
| ugv_base_ros | https://github.com/waveshareteam/ugv_base_ros | GitHub | ROS variant; CMD_ROS_CTRL T=13 (m/s, rad/s); serial transport; same GPIO base | README, source | Medium |
| ugv_rpi | https://github.com/waveshareteam/ugv_rpi | GitHub | Python upper-computer; JSON commands T=1 (speed), T=126 (IMU), T=127 (calibrate); heartbeat 3 s | Python client | High |
| ugv_rdk | https://github.com/waveshareteam/ugv_rdk | GitHub | RDK upper-computer; JSON-over-UART; same command set as ugv_rpi | Source | Medium |
| wave_rover_serial | https://github.com/msanterre/wave_rover_serial | GitHub | Python wrapper; serial command quirks; speed [-1,1] to motor | Python | Medium |
| waveshare-rover-development-setup | https://github.com/winetree94/waveshare-rover-development-setup | GitHub | Arduino IDE setup notes; no PlatformIO config found | README | Low |
| espressif/mcp-c-sdk | https://github.com/espressif/esp-iot-solution (component) | Component | SDK for on-device MCP server; tools only; resources/prompts not supported v1.x | include/*.h, src/ | High |
