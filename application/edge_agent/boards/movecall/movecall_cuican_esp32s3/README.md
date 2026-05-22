# Movecall CuiCan ESP32-S3

## Hardware Overview

| Feature | Specification |
|---------|---------------|
| Chip | ESP32-S3 |
| Flash | 8MB QIO 80MHz |
| PSRAM | Octal 80MHz |
| Display | GC9A01 240x240 round SPI |
| Audio Codec | ES8311 (I2S + I2C) |
| LED | WS2812 x1 (RMT + DMA) |
| Backlight | LEDC PWM control |

## GPIO Mapping

| Function | GPIO |
|----------|------|
| I2C SDA | 6 |
| I2C SCL | 7 |
| I2S MCLK | 45 |
| I2S BCLK | 39 |
| I2S WS | 41 |
| I2S DOUT | 42 |
| I2S DIN | 40 |
| SPI MOSI | 10 |
| SPI SCLK | 12 |
| LCD CS | 13 |
| LCD DC | 14 |
| LCD RST | 11 |
| PA Enable | 17 |
| Backlight | 16 |
| WS2812 | 21 |

## Build & Flash

```bash
cd application/edge_agent

# Set target chip
idf.py set-target esp32s3

# Generate board configuration
idf.py bmgr --customer-path ./boards -b movecall_cuican_esp32s3

# Build
idf.py build

# Flash (replace with your actual serial port)
idf.py -p /dev/cu.usbmodem2101 flash

# Monitor logs
idf.py -p /dev/cu.usbmodem2101 monitor
```

## Partition Table

Uses `partitions_8MB.csv` for the 8MB flash layout.

## Files

| File | Description |
|------|-------------|
| `board_info.yaml` | Board identity (chip, manufacturer) |
| `board_peripherals.yaml` | Peripheral pin and bus configuration |
| `board_devices.yaml` | Device driver configuration (audio, display, LED) |
| `sdkconfig.defaults.board` | Board-level sdkconfig defaults |
| `setup_device.c` | Display driver initialization entry point |
