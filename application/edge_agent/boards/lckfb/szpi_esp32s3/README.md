# LCKFB SZPI ESP32-S3

Board adaptation for **立创·实战派 ESP32-S3** (LCKFB-SZPI-ESP32-S3).

## Hardware Overview

| Feature | Specification |
|---------|---------------|
| Module | ESP32-S3-WROOM-1-N16R8 |
| Flash | 16 MB QIO 80 MHz |
| PSRAM | 8 MB Octal 80 MHz |
| Display | ST7789 2.0" IPS 320x240 LCD (SPI) |
| Touch | FT6336 capacitive touch (I2C) |
| Audio ADC | ES7210 4-ch ADC (2 mics + 1 playback reference) |
| Audio DAC | ES8311 (I2C) |
| Audio PA | NS4150B, enabled via PCA9557 `PA_EN` |
| Camera | GC0308 (0.3 MP) or GC2145 (2 MP), DVP, auto-detected |
| IMU | QMI8658 6-axis accel + gyro (I2C) |
| Storage | microSD (TF) in 1-bit SD mode |
| IO expander | PCA9557 @0x19 (LCD_CS / PA_EN / DVP_PWDN) |
| Button | BOOT (IO0) |

## GPIO Mapping

| Function | GPIO |
|----------|------|
| **Shared I2C** | SDA=1, SCL=2 |
| **LCD (ST7789, SPI3)** | MOSI=40, SCLK=41, DC=39, CS=PCA9557 IO0, RST=nc |
| **Backlight** | 42 (LEDC PWM, active low) |
| **Audio I2S** | MCLK=38, BCLK=14, WS=13, DOUT=45, DIN=12 |
| **Camera (GC0308 DVP)** | XCLK=5, PCLK=7, VSYNC=3, HREF=46, PWDN=PCA9557 IO2 |
| **Camera data D0..D7** | 16, 18, 8, 17, 15, 6, 4, 9 |
| **TF card (1-bit SD)** | CLK=47, CMD=48, D0=21 |
| **PCA9557** | IO0=LCD_CS, IO1=PA_EN, IO2=DVP_PWDN |

The PCA9557 expander is initialized first by `setup_device.c`: `LCD_CS` is held
low, the speaker amplifier is enabled and the camera is powered, so the display,
audio and camera devices need no extra board hooks.

> Note: the stock firmware keeps `PA_EN` low until playback. ESP-Claw holds
> `PA_EN` high permanently, because the board framework has no PA hook for an
> IO-expander pin.

The BOOT key (IO0, active low) is exposed as the BMGR device `boot_button`
(`type: button`, `sub_type: gpio`). The Lua `button` module can also create a
handle directly on GPIO0.

## Build & Flash

```bash
cd application/edge_agent

# Generate board support files (needs: pip install esp-bmgr-assist)
idf.py bmgr -c ./boards -b szpi_esp32s3

idf.py build
idf.py -p <PORT> flash monitor
```

Console output is available on both the on-board CH340K UART0 port and the
native USB Serial/JTAG port.

## Known Limitations

- Hardware bring-up of the display, touch, audio, camera, SD card and IMU has
  not been verified on a physical board yet in this repository. The QMI8658 IMU
  backend was added for this board; camera support uses the `esp_video`
  (claw_video) DVP path with both GC0308 and GC2145 sensor drivers enabled for
  auto-detection.

## Files

| File | Description |
|------|-------------|
| `board_info.yaml` | Board identity (chip, manufacturer) |
| `board_peripherals.yaml` | Peripheral pin and bus configuration |
| `board_devices.yaml` | Device driver configuration |
| `sdkconfig.defaults.board` | Board-level sdkconfig defaults |
| `setup_device.c` | PCA9557 expander init, ST7789 and FT6336 factories |
