# ATK-DNESP32S3-BOX0

Board support for the ALIENTEK ATK-DNESP32S3-BOX0.

This port follows the pin map used by `xiaozhi-esp32/main/boards/atk-dnesp32s3-box0`.
It declares power rails, battery/charging monitor, ST7789 LCD, LEDC backlight,
ES8311 audio input/output, and three GPIO buttons.

TF card support is intentionally not declared because the referenced Xiaozhi
BOX0 board files do not define TF card pins. Add it only after the BOX0
schematic or measured pin map is confirmed.

## Build

```bash
cd application/edge_agent
idf.py set-target esp32s3
idf.py gen-bmgr-config -c ./boards -b atk_dnesp32s3_box0
idf.py build
idf.py merge-bin -o build/atk_dnesp32s3_box0_merged.bin
```

If the active ESP-IDF does not provide `idf.py merge-bin`, use
`esptool.py merge_bin` with the generated build flash arguments.

## Boot checkpoints

- `BOX0 power manager initialized`
- FATFS mounted at `/fatfs`
- Wi-Fi manager starts
- Board manager initializes `display_lcd`, `lcd_brightness`, `audio_dac`, and `audio_adc`

## Hardware smoke test

- The ST7789 backlight turns on and the display shows the Agent UI or a Lua display demo.
- ES8311 playback and microphone input initialize without I2S or codec errors.
- Buttons are readable from Lua with:

```lua
local button = require("button")
local right = button.new(0, 0)
local middle = button.new(4, 0)
local left = button.new(3, 0)
```

- Battery logs show plausible ADC, percentage, and charging state values.
