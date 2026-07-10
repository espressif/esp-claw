# EDA Robot Pro

## Hardware Overview

| Feature | Specification |
|---------|---------------|
| Chip | ESP32-S3 |
| Flash | 16MB QIO 80MHz |
| PSRAM | Octal 80MHz |
| Display | SSD1306 128x64 OLED (I2C) |
| Microphone | INMP441 MEMS (I2S) |
| Speaker | MAX98357 Class-D (I2S) |
| Servos | 4x SG90 (四足) |
| Buttons | Boot (GPIO0) + Touch (GPIO14) |

## GPIO Mapping

| Function | GPIO |
|----------|------|
| **I2C (OLED)** | |
| SDA | 12 |
| SCL | 13 |
| **I2S Microphone (INMP441)** | |
| SCK | 16 |
| WS | 17 |
| DIN | 18 |
| **I2S Speaker (MAX98357)** | |
| BCLK | 39 |
| LRCK | 38 |
| DOUT | 40 |
| **Servos** | |
| Left Front | 47 |
| Left Rear | 21 |
| Right Front | 9 |
| Right Rear | 10 |
| **Buttons** | |
| Boot | 0 |
| Touch | 14 |

## Build & Flash

```bash
cd application/edge_agent

# Set target chip
idf.py set-target esp32s3

# Install board manager assistant (required for bmgr)
pip install esp-bmgr-assist

# Generate board configuration
idf.py bmgr --customer-path ./boards -b eda_robot_pro

# Build
idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash monitor
```

## Project Documentation

- [EDA-Robot-Pro 项目文档](https://wiki.lceda.cn/zh-hans/course-projects/smart-internet/eda-robot/eda-robot-introduce.html)

## Files

| File | Description |
|------|-------------|
| `board_info.yaml` | Board identity (chip, manufacturer) |
| `board_peripherals.yaml` | Peripheral pin and bus configuration |
| `board_devices.yaml` | Device driver configuration (audio, display) |
| `sdkconfig.defaults.board` | Board-level sdkconfig defaults |
