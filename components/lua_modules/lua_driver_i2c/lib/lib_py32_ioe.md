# lib_py32_ioe.lua

Reusable Lua driver for the PY32 based M5Stack I2C IO expander. It uses the builtin `i2c` module and exports `require("lib_py32_ioe")`.

## When to use

Use this library when a script needs the expander's GPIOs, ADC channels, PWM channels or addressable-LED block. On M5Stack StackChan this chip is the board's "IOE1" at address `0x6F`, where pin 0 is the servo power rail (`VM_EN`) and IO14 drives the 12 Ring RGB LEDs.

On StackChan the firmware already brings `VM_EN` high during board init, so scripts only need this library to change that rail, read the ADC channels or drive the LEDs.

## IO14 is muxed: GPIO or Neopixel

IO14 (logical pin `13`) defaults to GPIO, with the Neopixel output as its alternate function. The chip datasheet and the vendor `M5IOE1` library treat the two as mutually exclusive, so this library refuses GPIO calls (`set_dir`, `set_pull`, `set_drive`, `write`) on pin `13` while the LED count is non-zero, and tells you to call `disable_leds()` first.

`set_led_count(count)` with a non-zero count restores IO14 to its reset default (input, no pulls, push-pull) before arming the block. That matters because the expander is a separate MCU on its own rail: its register state survives a host reset, so whatever a previous session left in those bits is still there after re-flashing.

On the StackChan unit this driver was tested against, the strip lights whether or not IO14 is also configured as a GPIO output, so the exclusion is what the documentation specifies rather than something observed to break. If a board only works the other way, `force_neopixel_pin_output()` is the escape hatch.

## Loading

```lua
local py32_ioe = require("lib_py32_ioe")
```

The script must also have access to the `i2c` and `delay` modules. You can either pass an existing I2C bus handle or let the library create one from GPIO options.

## Constructor

```lua
local ioe = py32_ioe.new(opts)
```

`opts` is a table:

- `bus`: existing I2C bus userdata. Recommended when the script already owns a bus, or when the board manager already created it.
- `port`: I2C port number. Required if `bus` is not provided.
- `sda`: SDA GPIO. Required if `bus` is not provided.
- `scl`: SCL GPIO. Required if `bus` is not provided.
- `freq_hz`: I2C frequency in Hz. Defaults to `100000`.
- `frequency`: alias of `freq_hz`.
- `addr`: 7-bit I2C address. Defaults to `0x6F`.
- `close_bus`: set to `true` to let `ioe:close()` also close a bus the library created.
- `boot_timeout_ms`: how long to poll the version register while the PY32 boots. Defaults to `1200`.

The constructor raises an error if the version register never returns a valid value within `boot_timeout_ms`. The chip boots slower than the host SoC, which is why the poll exists.

## Module functions

- `py32_ioe.pin_count()`: returns `14` (P0..P13).
- `py32_ioe.led_max()`: returns `32`, the size of the LED RAM block.
- `py32_ioe.NEOPIXEL_PIN`: `13`, the only pin the LED engine can drive.
- `py32_ioe.rgb_to_565(r, g, b)`: converts 8-bit RGB to the RGB565 value the chip stores.

## Identity

- `ioe:address()`: returns the configured 7-bit I2C address.
- `ioe:version()`: reads the firmware version byte.
- `ioe:uid()`: reads the 16-bit device UID.

## GPIO

Pins are `0..13`.

- `ioe:set_dir(pin, is_output)`: `true` for output, `false` for input.
- `ioe:set_pull(pin, pull)`: `pull` is `"up"`, `"down"` or `"none"`.
- `ioe:set_drive(pin, open_drain)`: `false` selects push-pull, which LED data lines need.
- `ioe:write(pin, level)`: drive an output pin.
- `ioe:read(pin)`: read the input level, returns a boolean.
- `ioe:read_output(pin)`: read back the latched output level.
- `ioe:set_irq_enabled(pin, enabled)`: enable or disable the per-pin interrupt.
- `ioe:clear_irq()`: clear all latched interrupt status bits.

## ADC

- `ioe:analog_read(channel)`: `channel` is `1..4`. Starts a conversion, waits for the busy bit to clear and returns the raw result. Raises an error if the conversion does not finish within 100 ms.

## PWM

- `ioe:set_pwm_duty(channel, duty)`: `channel` is `0..3`, `duty` is `0..255` and is scaled to the chip's 12-bit resolution. Writing a duty also enables the channel.
- `ioe:set_pwm_frequency(freq_hz)`: sets the shared PWM frequency, `0..65535`.

## Addressable LEDs

Only IO14 (pin `13`) can drive a strip. The vendor sequence is three steps and configures no pin: set the count, write LED RAM, set the refresh flag. Colours are staged in the chip's RAM and only reach the strip on refresh.

- `ioe:set_led_count(count)`: `0..32`. Call this before setting colours.
- `ioe:led_count()`: returns the configured count.
- `ioe:disable_leds()`: sets the count to `0`, which turns the strip off and releases pin 13 for GPIO use.
- `ioe:force_neopixel_pin_output()`: escape hatch that configures IO14 as a push-pull output with a pull-up, the order the older M5Stack PY32IOExpander BSP uses. Call it after `set_led_count`; it bypasses the mutual-exclusion guard by design.
- `ioe:set_led_color(index, r, g, b)`: stage one LED, `index` starts at `0`.
- `ioe:set_led_color565(index, color565)`: stage one LED using a raw RGB565 value.
- `ioe:read_led_color565(index)`: read a staged colour back out of LED RAM. Useful to prove the chip accepted the write when the strip stays dark.
- `ioe:refresh_leds()`: flush the staged colours to the strip. The chip auto-clears the flag once the engine has run, so reading `led_config_raw()` afterwards tells you whether it did.
- `ioe:fill_leds(r, g, b)`: stage every configured LED to one colour and flush.
- `ioe:led_config_raw()` / `ioe:set_led_config_raw(value)`: raw `LED_CFG` access. Count in bits 0..5, refresh one-shot in bit 6.

Colours are stored as RGB565, so the low bits of each channel are dropped.

## Teardown

- `ioe:close()`: releases the I2C device handle, and the bus too when the library created it and `close_bus` was set.

## Example: toggle the StackChan servo rail and light the ring

```lua
local i2c = require("i2c")
local py32_ioe = require("lib_py32_ioe")
local delay = require("delay")

-- CoreS3 internal I2C bus; attaching to the port the board manager already
-- owns is safe.
local bus = i2c.new(0, 12, 11, 100000)
local ioe = py32_ioe.new({ bus = bus })

print(string.format("PY32 version 0x%02X uid 0x%04X", ioe:version(), ioe:uid()))

-- pin 0 = VM_EN: cut and restore servo power
ioe:set_dir(0, true)
ioe:write(0, false)
delay.delay_ms(200)
ioe:write(0, true)

-- pin 13 = IO14, the Neopixel output. set_led_count restores the pin to its
-- default before arming, so no pin configuration is needed here.
ioe:set_led_count(12)
ioe:fill_leds(0, 32, 0)

ioe:close()
bus:close()
```

## Notes

- Bit-level writes are read-modify-write, so each GPIO call costs one read plus one write. Batch LED updates through `fill_leds` or repeated `set_led_color` followed by a single `refresh_leds`.
- Pins `8..13` live in the high half of each register pair; the library handles that split, but note that only 14 of the 16 bits exist.
- Multi-byte access is only valid inside the chip's register blocks (`0x00-0x2F`, `0x30-0x6F`, `0x70-0x8F`, `0x90`). Every burst this library issues stays within one block; a burst that crosses a boundary is not supported by the chip.
- The chip has no software reset in this driver. Power-cycling the board is the only way to restore defaults.
