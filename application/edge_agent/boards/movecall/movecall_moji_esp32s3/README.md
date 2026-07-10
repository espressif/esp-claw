# Movecall Moji ESP32-S3

## Hardware Overview

| Feature | Specification |
|---------|---------------|
| Chip | ESP32-S3 |
| Flash | 16MB QIO 80MHz |
| PSRAM | Octal 80MHz |
| Display | GC9A01 240x240 round SPI |
| Audio Codec | ES8311 (I2S + I2C) |
| LED | WS2812 x1 (RMT + DMA) |
| Backlight | LEDC PWM control |

## GPIO Mapping

| Function | GPIO |
|----------|------|
| I2C SDA | 5 |
| I2C SCL | 4 |
| I2S MCLK | 6 |
| I2S BCLK | 14 |
| I2S WS | 12 |
| I2S DOUT | 11 |
| I2S DIN | 13 |
| SPI MOSI | 17 |
| SPI SCLK | 16 |
| LCD CS | 15 |
| LCD DC | 7 |
| LCD RST | 18 |
| PA Enable | 9 |
| Backlight | 3 |
| WS2812 | 21 |

## Build & Flash

```bash
cd application/edge_agent

# Set target chip
idf.py set-target esp32s3

# Generate board configuration
idf.py bmgr --customer-path ./boards -b movecall_moji_esp32s3

# Build
idf.py build

# Flash (replace with your actual serial port)
idf.py -p /dev/cu.usbmodem2101 flash

# Monitor logs
idf.py -p /dev/cu.usbmodem2101 monitor
```

## Differences from CuiCan

Moji shares the same display and audio codec as CuiCan. Key differences:

- Different GPIO pin assignments
- Larger flash (16MB vs 8MB)
- Different SPI data/clock pins

## Partition Table

Uses `partitions_16MB.csv` for the 16MB flash layout.

## Files

| File | Description |
|------|-------------|
| `board_info.yaml` | Board identity (chip, manufacturer) |
| `board_peripherals.yaml` | Peripheral pin and bus configuration |
| `board_devices.yaml` | Device driver configuration (audio, display, LED) |
| `sdkconfig.defaults.board` | Board-level sdkconfig defaults |
| `setup_device.c` | Display driver initialization entry point |
