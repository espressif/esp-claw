# Generic ESP32-S3 AI module (TFT, Xingzhi Cube 1.54" layout)

ESP32-S3R8 (16 MB flash, 8 MB octal PSRAM) module with INMP441 microphone,
MAX98357 amplifier + speaker, ST7789 240x240 TFT, WS2812 LED and a BOOT button.
A 4G ML307R modem and a TF card slot exist on the hardware but are not used.

The pinout follows the public xiaozhi-esp32 `xingzhi-cube-1.54tft-ml307`
board. Confirmed on the device: TFT backlight (GPIO13) and the WS2812 LED
(GPIO48). The rest is **not verified against a vendor schematic**.

| Function | GPIO |
|----------|------|
| Mic INMP441: WS / SCK / DIN | 4 / 5 / 6 |
| Amp MAX98357: DOUT / BCLK / LRCK | 7 / 15 / 16 |
| TFT SPI: MOSI(SDA) / CLK(SCL) / DC / RST / CS | 10 / 9 / 8 / 18 / 14 |
| TFT backlight | 13 |
| WS2812 | 48 |
| BOOT | 0 |
| ML307R modem RX / TX (unused) | 11 / 12 |
