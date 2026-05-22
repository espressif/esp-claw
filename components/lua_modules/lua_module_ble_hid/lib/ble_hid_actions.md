# ble_hid_actions.lua

Reusable pure-Lua BLE HID action helper for validating, normalizing, and
running agent-facing media, keyboard, and mouse actions.

## Require

```lua
local actions = require("ble_hid_actions")
```

## Dependencies

- `ble_hid` module
- An initialized and connected BLE HID session before calling `actions.run`

## Action Shape

Actions are Lua tables with a non-empty string `type` field.

Supported action types:
- `media`
- `keyboard_key`
- `keyboard_combo`
- `keyboard_text`
- `mouse_button`
- `mouse_move`
- `mouse_scroll`
- `keyboard`, deprecated legacy alias

## Functions

- `actions.normalize_action(action)`: return a normalized copy of an action, or `nil, err`.
- `actions.validate_action(action)`: normalize and validate an action, or return `nil, err`.
- `actions.run(action)`: validate an action and send it through `ble_hid`, or return `nil, err`.
- `actions.is_implemented(action_type)`: return whether an action type can be executed.
- `actions.action_types()`: return a sorted list of known action type names.

Validation errors are returned as `nil, err`. Errors raised by the underlying
`ble_hid` module, such as unsupported key names, remain Lua errors from
`ble_hid`.

## Actions

### Media

```lua
{ type = "media", key = "play_pause", gesture = "single" }
```

Fields:
- `key`: required media key string.
- `gesture`: optional gesture string, `single` by default.

Supported media keys are `volume_up`, `volume_down`, `play_pause`,
`next_track`, `previous_track`, and `mute`.

Supported gestures are `single`, `double`, and `long`.

### Keyboard Key

```lua
{ type = "keyboard_key", key = "ENTER" }
```

Fields:
- `key`: required keyboard key string.

Lowercase single letters and alphabetic key names are normalized to uppercase.
A single uppercase letter, for example `A`, is normalized to
`{ type = "keyboard_combo", keys = { "SHIFT", "A" } }` so it types the
uppercase character.

Keyboard keys include `A` through `Z`, `0` through `9`, `ENTER`, `ESC`,
`BACKSPACE`, `TAB`, `SPACE`, punctuation key names, `F1` through `F12`,
navigation keys, and arrow keys. See `ble_hid` module documentation for the
full supported key list.

### Keyboard Combo

```lua
{ type = "keyboard_combo", keys = { "CTRL", "C" } }
```

Fields:
- `keys`: required non-empty array of key or modifier strings.

Alphabetic key names are normalized to uppercase.

Supported modifiers are `CTRL`, `CONTROL`, `SHIFT`, `ALT`, `OPTION`, `GUI`,
`COMMAND`, `CMD`, `META`, `RIGHT_CTRL`, `RIGHT_SHIFT`, `RIGHT_ALT`, and
`RIGHT_GUI`.

### Keyboard Text

```lua
{ type = "keyboard_text", text = "hello" }
```

Fields:
- `text`: required string.

Text input uses the underlying `ble_hid.text(text)` behavior. It supports
printable ASCII, space, newline, and tab on a US keyboard layout. Unicode and
IME input are not supported.

### Mouse Button

```lua
{ type = "mouse_button", button = "left", gesture = "click" }
```

Fields:
- `button`: optional button string, `left` by default.
- `gesture`: optional gesture string, `click` by default.

Supported buttons are `left`, `right`, and `middle`.

Supported gestures are `click`, `down`, and `up`.

### Mouse Move

```lua
{ type = "mouse_move", dx = 30, dy = 0, scale = 1 }
```

Fields:
- `dx`: optional horizontal movement, `0` by default.
- `dy`: optional vertical movement, `0` by default.
- `scale`: optional multiplier, `1` by default.

Aliases:
- `x` may be used instead of `dx`.
- `y` may be used instead of `dy`.

`actions.run` sends `dx * scale` and `dy * scale` to `ble_hid.mouse_move`.

### Mouse Scroll

```lua
{ type = "mouse_scroll", vertical = -3, horizontal = 0 }
```

Fields:
- `vertical`: optional wheel movement, `0` by default.
- `horizontal`: optional horizontal pan movement, `0` by default.

Aliases:
- `wheel` may be used instead of `vertical`.
- `pan` may be used instead of `horizontal`.

### Legacy Keyboard

```lua
{ type = "keyboard", keys = { "CTRL", "C" } }
```

The legacy `keyboard` action is deprecated. It requires a non-empty `keys`
array and normalizes to either:
- `keyboard_key` when `keys` contains one entry.
- `keyboard_combo` when `keys` contains multiple entries.

## Example

```lua
local ble_hid = require("ble_hid")
local actions = require("ble_hid_actions")

local ok, err = ble_hid.init({ name = "esp-claw-hid" })
if not ok then
    error(err)
end

ok, err = ble_hid.start({ name = "esp-claw-hid" })
if not ok then
    error(err)
end

-- Pair and connect from the host Bluetooth settings before sending actions.

local action = {
    type = "keyboard_combo",
    keys = { "CTRL", "C" },
}

ok, err = actions.run(action)
if not ok then
    error(err)
end

actions.run({ type = "media", key = "play_pause" })
actions.run({ type = "keyboard_text", text = "hello" })
actions.run({ type = "mouse_move", dx = 20, dy = 0 })
actions.run({ type = "mouse_button", button = "left", gesture = "click" })
actions.run({ type = "mouse_scroll", vertical = -3 })

ble_hid.release_all()
ble_hid.stop()
ble_hid.deinit()
```
