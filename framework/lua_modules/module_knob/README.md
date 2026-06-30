# Lua Knob

This skill describes how to correctly use a rotary encoder (knob) when writing Lua scripts.

## How to call
- Import it with `local knob = require("knob")`
- Call `local handle = knob.new(gpio_a, gpio_b [, default_direction])` to create a knob handle
  - `gpio_a` and `gpio_b` are the two encoder signal pins
  - `default_direction`: 0 = positive increase (default), 1 = negative increase
- Call `knob.on(handle, event, callback)` to subscribe to a knob event
- Supported event names: `"left"`, `"right"`, `"h_lim"`, `"l_lim"`, `"zero"`
- Call `knob.off(handle [, event])` to remove a subscription or clear all callbacks
- Call `knob.dispatch()` to poll and dispatch pending knob events
- Call `knob.get_count(handle)` to read the current count value
- Call `knob.clear_count(handle)` to reset count to zero
- Call `knob.close(handle)` when the handle is no longer needed

## Events
| Event    | Description                    |
|----------|--------------------------------|
| `left`   | Rotated counter-clockwise      |
| `right`  | Rotated clockwise              |
| `h_lim`  | Count reached maximum limit    |
| `l_lim`  | Count reached minimum limit    |
| `zero`   | Count returned to zero         |

## Example
```lua
local knob = require("knob")

local handle = knob.new(48, 47)
knob.on(handle, "left", function(evt)
  print("left, count=" .. tostring(evt.count))
end)
knob.on(handle, "right", function(evt)
  print("right, count=" .. tostring(evt.count))
end)

knob.dispatch()
knob.close(handle)
```
