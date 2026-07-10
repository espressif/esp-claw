# Lua LCD

This module describes how to correctly use `lcd` when writing Lua scripts.

`lcd` is a panel bring-up module. It can:
- Initialize an SPI or QSPI LCD panel from Lua
- Create and destroy the panel device handle
- Reset the panel
- Return panel metadata for logging and follow-up setup

## How to call

- Import it with `local lcd = require("lcd")`
- Create a panel with `lcd.new(config)`
- Use the returned `panel_handle`, `io_handle`, `width`, `height`, and `panel_if` with `display.init(...)`
- Call `lcd.get_info(dev)` to inspect the created device
- Call `lcd.reset(dev)` when the panel needs to be reinitialized
- Call `lcd.delete(dev)` when finished

## Important rules

- `lcd.new(...)` only initializes the LCD bus, panel IO, and panel driver.
- This module does not manage backlight GPIO or brightness.
- If the board uses a dedicated backlight pin, control it separately with `gpio` or another module.
- The panel config must include `controller`, `bus`, `io`, `panel`, and `resolution` tables.
- `bus.mode` must be `"spi"` or `"qspi"`.
- `resolution.width` and `resolution.height` must be positive integers.
- `io.spi_mode` must be an integer in the range `0` to `3`.
- `panel.bits_per_pixel` currently accepts `16`, `18`, or `24`.
- QSPI is only valid for controllers that support it.

## Supported controllers

The current binding supports these `controller` names:
- `"gc9107"`
- `"gc9b71"`
- `"gc9d01"`
- `"nt35510"`
- `"co5300"`
- `"spd2010"`
- `"sh8601"`
- `"st7789"`
- `"st77916"`
- `"st77922"`

## Return values

`lcd.new(config)` returns:
- `dev`: the Lua LCD device userdata
- `panel_handle`: lightuserdata for the panel
- `io_handle`: lightuserdata for the panel IO
- `width`: panel width
- `height`: panel height
- `panel_if`: panel interface constant for `display.init(...)`

## Typical SPI example

```lua
local lcd = require("lcd")
local display = require("display")

local dev, panel_handle, io_handle, width, height, panel_if = lcd.new({
    controller = "st7789",
    bus = {
        host = 2,
        mode = "spi",
        sclk = 4,
        mosi = 5,
        max_transfer_sz = 6400,
    },
    io = {
        cs = 15,
        dc = 7,
        spi_mode = 0,
        pclk_hz = 40000000,
    },
    panel = {
        reset = 6,
        mirror_x = false,
        mirror_y = true,
        swap_xy = true,
        invert_color = true,
    },
    resolution = {
        width = 320,
        height = 240,
    }
})

local info = lcd.get_info(dev)
print(info.controller, info.width, info.height, info.bus_mode)

display.init(panel_handle, io_handle, width, height, panel_if)
```

## Config shape

### `bus`

- `host`: required SPI host integer
- `mode`: optional, `"spi"` by default
- `sclk`: required clock GPIO
- `mosi`: required for SPI mode
- `data0`, `data1`, `data2`, `data3`: required for QSPI mode
- `max_transfer_sz`: optional; if omitted or `0`, the module derives a full-frame transfer size

### `io`

- `cs`: required chip select GPIO
- `dc`: required in SPI mode
- `spi_mode`: optional SPI mode, default `0`
- `trans_queue_depth`: optional queue depth, default `10`
- `pclk_hz`: optional pixel clock; defaults depend on controller preset

### `panel`

- `reset`: optional reset GPIO, default `-1`
- `color_space`: optional `"rgb"` or `"bgr"`
- `bits_per_pixel`: optional, defaults depend on controller preset
- `reset_active_high`: optional boolean, default `false`
- `x_gap`: optional integer, default `0`
- `y_gap`: optional integer, default `0`
- `mirror_x`: optional boolean, default `false`
- `mirror_y`: optional boolean, default `false`
- `swap_xy`: optional boolean, default `false`
- `invert_color`: optional boolean, default `false`
- `disp_on`: optional boolean, default `true`

### `resolution`

- `width`: required integer
- `height`: required integer

## Device methods

### `lcd.get_info(dev)`

Returns a table with:
- `controller`
- `width`
- `height`
- `panel_if`
- `bus_mode`
- `host`
- `initialized`

### `lcd.reset(dev)`

Resets and reinitializes the panel.

### `lcd.delete(dev)`

Deletes the panel, panel IO, and SPI bus resources owned by this device.

## Backlight note

If a script also needs visible output, initialize the board backlight separately before or after `lcd.new(...)`, depending on the panel wiring. A typical pattern is:

```lua
local gpio = require("gpio")
gpio.set_direction(16, "output")
gpio.set_level(16, 0)
```

That GPIO logic is board-specific and is not part of the `lcd` module itself.
