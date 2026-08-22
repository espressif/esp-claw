# EDA TV Pro

## Hardware Overview

| Feature | Specification |
|---------|---------------|
| Chip | ESP32-S3 |
| Flash | 8MB QIO 80MHz |
| PSRAM | 无 |
| Display | ST7789 240x240 LCD (SPI) |
| Microphone | INMP441 MEMS (I2S) |
| Speaker | MAX98357 Class-D (I2S) |
| LED | 1x GPIO LED |
| Buttons | Boot (GPIO0) + Touch (GPIO40) + ASR (GPIO21) |

## GPIO Mapping

| Function | GPIO |
|----------|------|
| **SPI Display (ST7789)** | |
| MOSI | 11 |
| SCLK | 12 |
| DC | 47 |
| RST | 48 |
| CS | NC |
| Backlight | 38 |
| **I2S Microphone (INMP441)** | |
| SCK | 5 |
| WS | 4 |
| DIN | 6 |
| **I2S Speaker (MAX98357)** | |
| BCLK | 15 |
| LRCK | 16 |
| DOUT | 7 |
| **LED** | |
| Built-in LED | 2 |
| **Buttons** | |
| Boot | 0 |
| Touch | 40 |
| ASR | 21 |

## Build & Flash

```bash
cd application/edge_agent

# Set target chip
idf.py set-target esp32s3

# Install board manager assistant (required for bmgr)
pip install esp-bmgr-assist

# Generate board configuration
idf.py bmgr --customer-path ./boards -b eda_tv_pro

# Build
idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash monitor
```

## Project Documentation

- [EDA-TV-Pro 项目文档](https://wiki.lceda.cn/zh-hans/course-projects/smart-internet/tv-pro/tv-pro-introduce.html)

## Files

| File | Description |
|------|-------------|
| `board_info.yaml` | Board identity (chip, manufacturer) |
| `board_peripherals.yaml` | Peripheral pin and bus configuration |
| `board_devices.yaml` | Device driver configuration (audio, display, LED) |
| `sdkconfig.defaults.board` | Board-level sdkconfig defaults |
