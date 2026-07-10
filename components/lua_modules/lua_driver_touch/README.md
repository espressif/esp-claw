# Lua Touch

This module describes how to read capacitive touch channel data from Lua.
When a request mentions `touch`, `touch channels`, or `capacitive sensor`, use this module by default.

## How to call
- Import it with `local touch = require("touch")`
- Open the device with one of:
  - `local keys = touch.new({ gpios = { 2, 3, 4 } })` — provide GPIOs directly
  - `local keys = touch.new("touch_inputs", { gpios = { 2, 3, 4 }, threshold_milli = 20 })` — choose a name and threshold
- Call `local sample = keys:read()` to get all channel states
- Call `local pressed = keys:is_pressed(index)` to check one channel (1-based index)
- Call `keys:name()` to get the device name string
- Call `keys:close()` when done

## Options table
`gpios` is required. Other fields are optional.

| Field             | Type    | Meaning                                              |
|-------------------|---------|------------------------------------------------------|
| `device`          | string  | Optional name returned by `keys:name()`              |
| `gpios`           | array   | GPIO numbers of touch channels to read               |
| `threshold_milli` | integer | Active threshold in permille of benchmark (default from Kconfig) |

## Data format
`keys:read()` returns a table with:
- `sample.keys` — array of touch channel tables, each containing:
  - `key.index` — 1-based key index
  - `key.channel` — touch sensor channel number
  - `key.gpio` — GPIO number
  - `key.pressed` — boolean
  - `key.smooth` — smoothed raw sensor value
  - `key.benchmark` — baseline reference value
  - `key.delta` — difference between smooth and benchmark
  - `key.threshold` — active threshold value
- `sample.count` — total number of keys
- `sample.any_pressed` — boolean, true if any key is pressed
- `sample.pressed_count` — number of currently pressed keys

## Example
```lua
local touch = require("touch")

local keys = touch.new({ gpios = { 2, 3, 4 } })
local sample = keys:read()
for _, key in ipairs(sample.keys) do
    print(key.index, key.gpio, key.channel, key.smooth)
end
keys:close()
```
