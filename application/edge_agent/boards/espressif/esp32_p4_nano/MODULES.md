# ESP32-P4-NANO 模块与引脚汇总

> 本文档基于 ESP32-P4-NANO.pdf（schematics）与现有 board_* 文件整理，按模块逐一列出 GPIO 映射关系。

## 1. MicroSD 卡槽（MicroSD / SD1）

ESP32-P4-NANO 板载了一个 4-Wire SDIO 3.0 卡槽，可拓展片外存储。

支持的速率模式：

- 默认速率（20 MHz）
- 高速模式（40 MHz）

**SDMMC 4 线连接定义**：

| 信号 | GPIO | 说明     |
| ---- | ---- | -------- |
| CLK  | 43   | 时钟线   |
| CMD  | 44   | 命令线   |
| D0   | 39   | 数据线 0 |
| D1   | 40   | 数据线 1 |
| D2   | 41   | 数据线 2 |
| D3   | 42   | 数据线 3 |

配置参考（ESP-IDF）：

```c
sdmmc_host_t host = SDMMC_HOST_DEFAULT();
host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // 40 MHz

sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
slot_config.width = 4;
slot_config.clk = 43;
slot_config.cmd = 44;
slot_config.d0 = 39;
slot_config.d1 = 40;
slot_config.d2 = 41;
slot_config.d3 = 42;
slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
```

## 2. 音频子系统（Codec — ES8311）

- 芯片：ES8311
- 控制总线：I2C
  - SDA: GPIO7
  - SCL: GPIO8
  - I2C 地址：0x18（7-bit）/ board_manager 使用 0x30（8-bit，0x18<<1）
- 音频数据（I2S）：
  - MCLK: GPIO13
  - BCLK (SCLK): GPIO12
  - WS (LRCK): GPIO10
  - ASDOUT (播放输出): GPIO11
  - DSDIN (录音输入): GPIO9
- 功放/PA 控制：GPIO53（高电平有效）

> 组件依赖：`idf.py add-dependency "espressif/es8311"`

## 3. 摄像头 / 显示接口（CSI / DSI）

板上没有 SPI 驱动的 ST7789 显示；原理图实际走的是 MIPI CSI / DSI 差分链路。

PDF 中存在大量 NLCSI / NLDSI 前缀信号（如 `NLCSI0CLK0P/N`、`NLDSI0D00P/N`），这些为高速差分对信号：

- **CSI 差分信号组**：NLCSI0CLK0P/N、NLCSI0D00/10P/N、NLCSI0IOx 等（详见 PDF 的 CSI 原理图块）
- **DSI 差分信号组**：NLDSI0CLK0P/N、NLDSI0D00/10P/N 等（详见 PDF 的 DSI 原理图块）

> 如需支持摄像头或 MIPI 显示，需逐页对照原理图将每个差分对映射为 SoC 的 CSI/DSI 引脚，并在 `board_peripherals.yaml` 中添加 `dvp` / `mipi` / `parallel` 配置。

## 4. 按键 & 指示灯

- 按键（GPIO 输入，低有效）：
  - BOOT（GPIO35）：上电或复位时按下，进入下载模式
  - RESET（ESP_EN）：系统复位
- USER-LED（GPIO23）：电源/系统指示灯，高有效

## 5. USB OTG 2.0 高速接口

- 接口标准：USB 2.0 OTG，支持 480Mbps（High Speed）
- 物理接口：Type-A 连接器
  - 用途：可作为 Host 连接 USB 设备，或作为 Device 被其他主机识别
  - GPIO 映射：ESP32-P4 内部集成 USB OTG 控制器，引脚由 SoC 内部管理（无需外部 GPIO 配置）
  - SDK 配置：`USB_SERIAL_JTAG` 等选项影响 USB 功能，需按实际需求调整

## 6. 以太网接口（RJ45 网口 + PHY IP101GRI）

- 芯片：IP101GRI（百兆 PHY）
- MAC：集成在 ESP32-P4 内部
- 连接方式：RMII（Reduced MII）
- 物理接口：RJ45（标准网线接口）
- 速率：10/100 Mbps 自适应

**RMII 引脚映射**（参考 Waveshare 官方示例）：

| 信号    | GPIO | 说明                |
| ------- | ---- | ------------------- |
| TXD0    | 34   | 发送数据 0          |
| TXD1    | 35   | 发送数据 1          |
| RXD0    | 30   | 接收数据 0          |
| RXD1    | 29   | 接收数据 1          |
| TX_EN   | 49   | 发送使能            |
| CRS_DV  | 28   | 载波检测 / 数据有效 |
| REF_CLK | 50   | 参考时钟（50MHz）   |
| MDIO    | 52   | 管理数据            |
| MDC     | 31   | 管理时钟            |
| RESET   | 51   | PHY 复位            |

> 备注：PoE 模块接口预留（可选配），支持 802.3af PoE（37-57V 输入，5V 2.5A 输出）。

## 7. WiFi 联网（ESP32-C6 作为协处理器）

ESP32-P4 本身不带 WiFi/BT 功能，ESP32-P4-NANO 通过 **SDIO** 连接了一块 **ESP32-C6-MINI-1** 模组来拓展 WiFi 6 / Bluetooth 5 功能。

### 架构说明

- **P4（Host）**：主芯片，运行应用逻辑
- **C6（Slave）**：协处理器，通过 SDIO 提供 WiFi 6 / BT 5 能力
- **关键组件**：
  - `espressif/esp_hosted`：主机端框架，负责 RPC / transport 与协处理器通信
  - `espressif/esp_wifi_remote`：WiFi Remote 组件，基于 esp_hosted 实现无缝 `esp_wifi` API

### SDIO 连接映射（P4 Host ← → C6 Slave）

| 信号 | GPIO | CONFIG 变量 | 说明 |
|------|:----:|------|------|
| CMD | 19 | `CONFIG_ESP_HOSTED_SDIO_PIN_CMD` | SDIO 命令线 |
| CLK | 18 | `CONFIG_ESP_HOSTED_SDIO_PIN_CLK` | SDIO 时钟线 |
| D0 | 14 | `CONFIG_ESP_HOSTED_SDIO_PIN_D0` | SDIO 数据线 0 |
| D1 | 15 | `CONFIG_ESP_HOSTED_SDIO_PIN_D1` | SDIO 数据线 1（4-bit 模式） |
| D2 | 16 | `CONFIG_ESP_HOSTED_SDIO_PIN_D2` | SDIO 数据线 2（4-bit 模式） |
| D3 | 17 | `CONFIG_ESP_HOSTED_SDIO_PIN_D3` | SDIO 数据线 3（4-bit 模式） |
| RESET | 54 | `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE` | C6 复位信号 |

**SDIO 配置参数**：

| 参数 | 值 | CONFIG 变量 |
|------|:--:|------|
| Slot | 1 | `CONFIG_ESP_HOSTED_SDIO_SLOT` |
| 总线宽度 | 4-bit | `CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS` |
| 时钟频率 | 40 MHz | `CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ` |
| 复位电平 | Active High | `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH` |
| 复位延迟 | 1500 ms | `CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS` |
| 启动复位 | 每次 Host 启动 | `CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP` |

### Slot 分配

- **Slot 0**：MicroSD 卡槽（GPIO43/44/39–42）
- **Slot 1**：ESP32-C6 协处理器（GPIO14–19, GPIO54）

两个 Slot 使用不同 GPIO 组，无引脚冲突。

### 软件组件

```bash
idf.py add-dependency "espressif/esp_wifi_remote"
idf.py add-dependency "espressif/esp_hosted"
```

### C6 模组关键引脚

- **C6_U0TXD / C6_U0RXD**：UART 烧录接口（用于给 C6 单独烧录固件）
- **IO9**：上电时拉低可让 C6 进入下载模式
- **CHIP_PU（EN）**：C6 芯片使能脚

### 备用传输接口（未启用）

SDIO 是当前唯一启用的 Host↔Slave 传输方式。以下接口在 SDK 中存在配置选项但未启用：
- SPI HD / SPI 主机接口
- UART 主机接口

## 8. Type-C UART 接口（烧录 / 调试口）

- 物理接口：USB Type-C 连接器
- 功能：
  - 供电（5V 电源输入）
  - UART 烧录与调试（通过 USB-to-UART 转换）
- USB-to-UART 芯片：CH343P
- UART 引脚映射：
  - RXD: GPIO37
  - TXD: GPIO38
- 用途：程序烧录、Serial Monitor 调试、日志查看

## 9. GPIO 扩展接口（P1 / P2）

板载两路 2×13 Pin Header（2.54mm 间距），共引出 28 个可编程 GPIO。

### P1（左侧排针）

| Pin | 信号             |  | Pin | 信号              |
| :-: | ---------------- | - | :-: | ----------------- |
|  1  | 3V3              |  |  2  | 5V                |
|  3  | GPIO7  (I2C SDA) |  |  4  | 5V                |
|  5  | GPIO8  (I2C SCL) |  |  6  | GND               |
|  7  | GPIO23 (LED)     |  |  8  | GPIO37 (UART RXD) |
|  9  | GND              |  | 10 | GPIO38 (UART TXD) |
| 11 | GPIO5            |  | 12 | GPIO4             |
| 13 | GPIO20           |  | 14 | GND               |
| 15 | GPIO21           |  | 16 | GPIO22            |
| 17 | 3V3              |  | 18 | GPIO24            |
| 19 | GPIO25           |  | 20 | GND               |
| 21 | GPIO26           |  | 22 | GPIO27            |
| 23 | GPIO32           |  | 24 | GPIO33            |
| 25 | GND              |  | 26 | GPIO36            |

### P2（右侧排针）

| Pin | 信号              |  | Pin | 信号                 |
| :-: | ----------------- | - | :-: | -------------------- |
|  1  | 5V                |  |  2  | 5V                   |
|  3  | GND               |  |  4  | GND                  |
|  5  | 3V3               |  |  6  | GPIO0                |
|  7  | GND               |  |  8  | GPIO1                |
|  9  | GPIO3             |  | 10 | GND                  |
| 11 | GPIO2             |  | 12 | GPIO6                |
| 13 | GPIO54 (C6 RESET) |  | 14 | GPIO53 (PA 控制)     |
| 15 | GPIO47            |  | 16 | GPIO48               |
| 17 | GPIO46            |  | 18 | GND                  |
| 19 | GPIO45            |  | 20 | C6_U0RXD             |
| 21 | C6_IO12           |  | 22 | C6_U0TXD             |
| 23 | C6_IO13           |  | 24 | C6_IO9 (C6 下载模式) |
| 25 | GND               |  | 26 | GND                  |

> **注意**：P2 引出部分 ESP32-C6 协处理器引脚（C6_IOx / C6_U0），可用于 C6 独立烧录或调试。

## 不确定 / 待确认项

- MIPI CSI/DSI 信号组尚未逐对映射为具体 GPIO，如需使用摄像头或 DSI 显示，需对照原理图完善 `board_peripherals.yaml`。
- 部分外设（如 PoE 模块）的使能/控制脚需参照完整原理图确认。

## 参考

- 源文件：`boards/espressif/esp32_p4_nano/ESP32-P4-NANO.pdf`（schematics）
- 已核对文件：
  - `board_peripherals.yaml`
  - `board_devices.yaml`
  - `setup_device.c`
  - `sdkconfig` / `sdkconfig.json`（SDIO / ESP_HOSTED 配置项）
