# Lua LCD

LCD panel creation is currently unavailable through this module. When the module is enabled, `require("lcd")` succeeds, but `lcd.new(...)` always raises a Lua error and does not return a usable device.

Changing constructor arguments does not enable hardware access.

Use `display.open()` for drawing on the built-in screen.

## Check availability

```lua
local lcd = require("lcd")
local ok, err = pcall(lcd.new)
if not ok then
    print(err)
end
```
