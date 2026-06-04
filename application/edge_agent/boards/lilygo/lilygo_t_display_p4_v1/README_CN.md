# Lilygo T-Display-P4 板级支持说明

## **[English](./README.md) | 中文**

上游板级仓库：[T-Display-P4](https://github.com/Xinyuan-LilyGO/T-Display-P4)

## 概览

当前配置按照 ESP Board Manager v0.5.12 的设备模型组织。XL9535 的电源和复位
引脚由标准 `gpio_expander` 设备在 YAML 中配置。`setup_device.c` 在创建屏幕前只
探测 GT9895 触摸 ID：探测到 GT9895 就选择 RM69A10 AMOLED 路径，没有探测到则
回退到 HI8561 LCD 路径。HI8561 触摸会在 LCD 面板初始化完成后再创建，因为它的
触摸信息地址需要从 HI8561 控制器中读取。

| 设备名 | 作用 |
| --- | --- |
| `gpio_expander_xl9535` | 通过 YAML 拉起 Power_EN_3V3、Power_EN_5V0、Screen_RST、Touch_RST 和 Vcca_EN。 |
| `display_lcd` | 根据 GT9895 探测结果，在运行时选择 RM69A10 或 HI8561 的 DSI 时序。 |
| `lcd_touch` | 在屏幕型号确定后创建 GT9895 (`0xba`/`0x28`) 或 HI8561 (`0xd0`) 触摸。 |
| `lcd_brightness` | 使用 Board Manager `ledc_ctrl`，通过 GPIO51 控制 HI8561 的 LEDC 背光；RM69A10 亮度在 `esp_lcd_rm69a10.c` 中通过 DCS `0x51` 设置。 |
| `audio_dac` | 通过 Board Manager `audio_codec` 初始化 ES8311 播放路径。 |
| `audio_adc` | 通过 Board Manager `audio_codec` 初始化 ES8311 录音路径。 |

## 目录结构

| 文件 | 说明 |
| --- | --- |
| `board_info.yaml` | 板名、芯片、厂商和描述信息。 |
| `board_devices.yaml` | Board Manager 设备列表和组件依赖。 |
| `board_peripherals.yaml` | 可复用的 I2C、I2S、DSI、LDO、LEDC、SPI 和 UART 外设。 |
| `sdkconfig.defaults.board` | Flash、PSRAM、显示、Hosted Wi-Fi 和摄像头默认配置。 |
| `setup_device.c` | XL9535、DSI LCD 面板选择、触摸驱动选择的板级 factory entry。 |
| `esp_lcd_hi8561.c` | HI8561 DSI LCD 面板驱动和初始化序列。 |
| `esp_lcd_rm69a10.c` | RM69A10 DSI LCD 面板驱动和初始化序列。 |
| `esp_lcd_touch_hi8561.c` | 暴露 `esp_lcd_touch_handle_t` 的 HI8561 触摸驱动。 |
| `esp_lcd_touch_gt9895.c` | 暴露 `esp_lcd_touch_handle_t` 的 GT9895 触摸驱动。 |

## 快速开始

进入 `application/edge_agent` 后运行：

```powershell
python managed_components/espressif__esp_board_manager/gen_bmgr_config_codes.py -b ./boards/lilygo/lilygo_t_display_p4_v1 -c ./boards --project-dir .
```

生成结果会输出到：

```text
components/gen_bmgr_codes
```

> [!IMPORTANT]
> 命令执行前需要存在 `managed_components\espressif__esp_board_manager`，否则会生成失败。
