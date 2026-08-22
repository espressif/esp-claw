# EDA AI GameBox

## Hardware Overview

| Feature | Specification |
|---------|---------------|
| Chip | ESP32-S3 R8N8 |
| Flash | 8MB QIO 80MHz |
| PSRAM | 8MB Octal 80MHz |
| Display | ST7789 240x320 LCD (SPI) |
| Controls | 五向按键 + XYAB 按键 (共9键) |
| Buzzer | 无源蜂鸣器 (GPIO48, LEDC PWM) |
| Audio | 无 |

## GPIO Mapping

| Function | GPIO |
|----------|------|
| **SPI Display (ST7789)** | |
| MOSI | 13 |
| SCLK | 14 |
| CS | 11 |
| DC | 12 |
| RST | -1 (EN) |
| Backlight | 10 |
| **五向按键** | |
| 上 (Front) | 5 |
| 下 (Back) | 3 |
| 左 (Left) | 1 |
| 右 (Right) | 4 |
| 中 (Center) | 2 |
| **XYAB 按键** | |
| X | 8 |
| Y | 6 |
| A | 9 |
| B | 7 |
| **Buzzer** | |
| Passive Buzzer | 48 |

## Build & Flash

```bash
cd application/edge_agent

# Set target chip
idf.py set-target esp32s3

# Install board manager assistant (required for bmgr)
pip install esp-bmgr-assist

# Generate board configuration
idf.py bmgr --customer-path ./boards -b eda_ai_gamebox

# Build
idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash monitor
```

## Project Documentation

- [EDA-AI-GameBox 项目文档](https://wiki.lceda.cn/zh-hans/course-projects/smart-internet/eda-ai-gamebox/eda-ai-gamebox-introduce.html)

## Files

| File | Description |
|------|-------------|
| `board_info.yaml` | Board identity (chip, manufacturer) |
| `board_peripherals.yaml` | Peripheral pin and bus configuration |
| `board_devices.yaml` | Device driver configuration (display, buttons) |
| `sdkconfig.defaults.board` | Board-level sdkconfig defaults |
