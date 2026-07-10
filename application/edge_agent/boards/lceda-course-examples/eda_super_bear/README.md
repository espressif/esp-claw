# EDA Super Bear

## Hardware Overview

| Feature | Specification |
|---------|---------------|
| Chip | ESP32-S3 |
| Flash | 16MB QIO 80MHz |
| PSRAM | Octal 80MHz |
| Display | 无 |
| Microphone | INMP441 MEMS (I2S) |
| Speaker | MAX98357 Class-D (I2S) |
| Servos | 6x SG90 (双手双脚) |
| Buttons | Boot (GPIO38) + Touch (GPIO14) |

## GPIO Mapping

| Function | GPIO |
|----------|------|
| **I2S Microphone (INMP441)** | |
| SCK | 39 |
| WS | 40 |
| DIN | 41 |
| **I2S Speaker (MAX98357)** | |
| BCLK | 48 |
| LRCK | 45 |
| DOUT | 47 |
| **Servos** | |
| Left Leg | 13 |
| Left Foot | 12 |
| Right Leg | 11 |
| Right Foot | 14 |
| Left Hand | 21 |
| Right Hand | 10 |
| **Buttons** | |
| Boot | 38 |
| Touch | 14 |

## Build & Flash

```bash
cd application/edge_agent

# Set target chip
idf.py set-target esp32s3

# Install board manager assistant (required for bmgr)
pip install esp-bmgr-assist

# Generate board configuration
idf.py bmgr --customer-path ./boards -b eda_super_bear

# Build
idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash monitor
```

## Project Documentation

- [EDA-Super-Bear 项目文档](https://wiki.lceda.cn/zh-hans/course-projects/smart-internet/eda-superbear/eda-superbear-introduce.html)

## Files

| File | Description |
|------|-------------|
| `board_info.yaml` | Board identity (chip, manufacturer) |
| `board_peripherals.yaml` | Peripheral pin and bus configuration |
| `board_devices.yaml` | Device driver configuration (audio) |
| `sdkconfig.defaults.board` | Board-level sdkconfig defaults |
