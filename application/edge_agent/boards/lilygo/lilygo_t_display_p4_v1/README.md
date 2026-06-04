# Lilygo T-Display-P4 Board Support

## **English | [中文](./README_CN.md)**

Upstream board repository:
[T-Display-P4](https://github.com/Xinyuan-LilyGO/T-Display-P4)

## Overview

This board follows the ESP Board Manager v0.5.12 device model. The XL9535 power
rails and reset lines are configured by the standard `gpio_expander` device.
`setup_device.c` probes only the GT9895 touch ID before panel creation: GT9895
present selects the RM69A10 AMOLED path, and GT9895 absent falls back to the
HI8561 LCD path. HI8561 touch is created after the LCD panel is initialized
because its touch address information is read from the HI8561 controller.

| Device name | Purpose |
| --- | --- |
| `gpio_expander_xl9535` | Drives Power_EN_3V3, Power_EN_5V0, Screen_RST, Touch_RST, and Vcca_EN from YAML. |
| `display_lcd` | Selects RM69A10 or HI8561 DSI timing at runtime from the GT9895 probe result. |
| `lcd_touch` | Creates GT9895 (`0xba`/`0x28`) or HI8561 (`0xd0`) touch after the panel variant is known. |
| `lcd_brightness` | Uses Board Manager `ledc_ctrl` on GPIO51 for the HI8561 LEDC backlight path; RM69A10 brightness is set by DCS `0x51` in `esp_lcd_rm69a10.c`. |
| `audio_dac` | Initializes the ES8311 playback path through Board Manager `audio_codec`. |
| `audio_adc` | Initializes the ES8311 capture path through Board Manager `audio_codec`. |

## Directory Layout

| File | Description |
| --- | --- |
| `board_info.yaml` | Board name, chip, manufacturer, and description metadata. |
| `board_devices.yaml` | Board Manager device list and component dependencies. |
| `board_peripherals.yaml` | Reusable I2C, I2S, DSI, LDO, LEDC, SPI, and UART interfaces. |
| `sdkconfig.defaults.board` | Default configuration for flash, PSRAM, display support, Hosted Wi-Fi, and camera. |
| `setup_device.c` | Board factory entries for XL9535, DSI LCD panel selection, and touch driver selection. |
| `esp_lcd_hi8561.c` | HI8561 DSI LCD panel driver and init sequence. |
| `esp_lcd_rm69a10.c` | RM69A10 DSI LCD panel driver and init sequence. |
| `esp_lcd_touch_hi8561.c` | HI8561 touch driver that exposes an `esp_lcd_touch_handle_t`. |
| `esp_lcd_touch_gt9895.c` | GT9895 touch driver that exposes an `esp_lcd_touch_handle_t`. |

## Quick Start

Run this command from `application/edge_agent`:

```powershell
python managed_components/espressif__esp_board_manager/gen_bmgr_config_codes.py -b ./boards/lilygo/lilygo_t_display_p4_v1 -c ./boards --project-dir .
```

Generated files are written to:

```text
components/gen_bmgr_codes
```

> [!IMPORTANT]
> `managed_components\espressif__esp_board_manager` must exist before running
> this command, otherwise generation will fail.
