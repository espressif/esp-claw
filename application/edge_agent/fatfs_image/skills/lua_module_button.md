# Lua Button

This skill describes how to correctly use button when writing Lua scripts.

## How to call
- Import it with `local button = require("button")`
- Call `local handle = button.new(gpio_num [, active_level [, long_press_ms [, short_press_ms]]])` to create a button handle
- Call `button.on(handle, event, callback)` to subscribe to a button event
- Supported event names include `press_down`, `press_up`, `single_click`, `double_click`, `multiple_click`, `long_press_start`, `long_press_hold`, and `long_press_up`
- Call `button.off(handle [, event])` to remove a subscription or clear callbacks
- Call `button.dispatch()` to poll and dispatch pending button events
- Call `button.get_key_level(handle)` to read the current key level
- Call `button.close(handle)` when the handle is no longer needed

## Example
```lua
local button = require("button")

local handle = button.new(0, 0)
button.on(handle, "single_click", function(evt)
  print(evt.event, evt.repeat_count)
end)

button.dispatch()
button.close(handle)
```
