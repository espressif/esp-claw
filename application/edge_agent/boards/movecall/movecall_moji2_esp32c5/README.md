# Movecall Moji2 ESP32-C5

## Hardware Overview

| Feature | Specification |
|---------|---------------|
| Chip | ESP32-C5 |
| Flash | 16MB QIO 80MHz |
| PSRAM | Quad 80MHz |
| Display | ST77916 360x360 QSPI round |
| Audio Codec | ES8311 (I2S + I2C) |
| LED | WS2812 x1 (RMT, no DMA) |
| Backlight | LEDC PWM control |

## GPIO Mapping

| Function | GPIO |
|----------|------|
| I2C SDA | 26 |
| I2C SCL | 27 |
| I2S MCLK | 25 |
| I2S BCLK | 11 |
| I2S WS | 24 |
| I2S DOUT | 23 |
| I2S DIN | 12 |
| QSPI D0 | 9 |
| QSPI D1 | 8 |
| QSPI D2 | 7 |
| QSPI D3 | 6 |
| QSPI SCLK | 0 |
| LCD CS | 3 |
| LCD RST | 1 |
| PA Enable | 5 |
| Backlight | 2 |
| WS2812 | 10 |

## Build & Flash

```bash
cd application/edge_agent

# Set target chip
idf.py set-target esp32c5

# Generate board configuration
idf.py bmgr --customer-path ./boards -b movecall_moji2_esp32c5

# Build
idf.py build

# Flash (replace with your actual serial port)
idf.py -p /dev/cu.usbmodem2101 flash

# Monitor logs
idf.py -p /dev/cu.usbmodem2101 monitor
```

## Notes

- Display uses QSPI interface (4 data lines) with 32-bit `lcd_cmd_bits`
- `setup_device.c` contains the full ST77916 vendor initialization sequence (~160 register commands)
- RMT does not support DMA on C5 (`with_dma: false`)
- If flash size defaults to 2MB after `set-target`, verify that `sdkconfig.defaults.board` is applied, or manually set flash size to 16MB via `idf.py menuconfig`

## Partition Table

Uses `partitions_16MB.csv` for the 16MB flash layout.

## Files

| File | Description |
|------|-------------|
| `board_info.yaml` | Board identity (chip, manufacturer) |
| `board_peripherals.yaml` | Peripheral pin and bus configuration (QSPI) |
| `board_devices.yaml` | Device driver configuration (audio, display, LED) |
| `sdkconfig.defaults.board` | Board-level sdkconfig defaults |
| `setup_device.c` | ST77916 display driver initialization with vendor command sequence |
