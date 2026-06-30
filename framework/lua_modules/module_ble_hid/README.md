# Lua BLE HID

This module describes how to correctly use BLE HID when writing Lua scripts.
It exposes the device as a composite BLE HID peripheral with media control,
keyboard, and mouse input reports.

## How to call
- Import it with `local ble_hid = require("ble_hid")`
- Call `ble_hid.init([{ name = "esp-claw-hid" }])` before advertising or sending reports
- Call `ble_hid.start([{ name = "esp-claw-hid" }])` to start HID advertising
- Pair from the host operating system Bluetooth settings with the advertised name
- Call `ble_hid.status()` to read `{ initialized, advertising, connected, bonded }`
- Call `ble_hid.media(key [, gesture])` to send a Consumer Control report
- Call `ble_hid.key(key)` to press and release one keyboard key
- Call `ble_hid.combo(key_or_modifier, ...)` to press a shortcut such as `CTRL+C`
- Call `ble_hid.text(text)` to type printable ASCII text on a US keyboard layout
- Call `ble_hid.mouse_move(dx, dy [, wheel [, pan]])` to move the mouse
- Call `ble_hid.mouse_scroll(wheel [, pan])` to scroll vertically or horizontally
- Call `ble_hid.mouse_button(button [, gesture])` to click, press, or release a mouse button
- Call `ble_hid.release_all()` before stopping or after interrupted pointer/key actions
- Call `ble_hid.stop()` to stop advertising, and `ble_hid.deinit()` to release the HID stack

`init` and `start` accept an optional `name` field. The name length must be 29
bytes or less. The default name is `esp-claw-hid`.

## Example
```lua
local ble_hid = require("ble_hid")

local ok, err = ble_hid.init({ name = "esp-claw-hid" })
if not ok then
    error(err)
end

ok, err = ble_hid.start({ name = "esp-claw-hid" })
if not ok then
    error(err)
end

print("Pair with esp-claw-hid from the host Bluetooth settings")

local status = ble_hid.status()
print("connected:", status.connected, "bonded:", status.bonded)

ble_hid.media("play_pause")
ble_hid.media("volume_up")
ble_hid.key("ENTER")
ble_hid.combo("CTRL", "C")
ble_hid.text("hello")
ble_hid.mouse_move(20, 0)
ble_hid.mouse_button("left", "click")
ble_hid.mouse_scroll(-3, 0)

ble_hid.release_all()
ble_hid.stop()
ble_hid.deinit()
```

## Supported Input

Media keys:
- `volume_up`
- `volume_down`
- `play_pause`
- `next_track`
- `previous_track`
- `mute`

Media gestures:
- `single`
- `double`
- `long`

Keyboard keys include:
- `A` through `Z`
- `0` through `9`
- `ENTER`, `ESC`, `BACKSPACE`, `TAB`, `SPACE`
- `MINUS`, `EQUAL`, `LEFT_BRACKET`, `RIGHT_BRACKET`, `BACKSLASH`
- `SEMICOLON`, `QUOTE`, `GRAVE`, `COMMA`, `PERIOD`, `SLASH`
- `CAPS_LOCK`, `F1` through `F12`
- `PRINT_SCREEN`, `SCROLL_LOCK`, `PAUSE`, `INSERT`, `HOME`
- `PAGE_UP`, `DELETE`, `END`, `PAGE_DOWN`
- `RIGHT`, `LEFT`, `DOWN`, `UP`

Keyboard modifiers for `combo`:
- `CTRL`, `CONTROL`
- `SHIFT`
- `ALT`, `OPTION`
- `GUI`, `COMMAND`, `CMD`, `META`
- `RIGHT_CTRL`, `RIGHT_SHIFT`, `RIGHT_ALT`, `RIGHT_GUI`

Mouse buttons:
- `left`
- `right`
- `middle`

Mouse button gestures:
- `click`
- `down`
- `up`

`ble_hid.text(text)` simulates basic ASCII on a standard US keyboard layout. It
supports letters, digits, common printable punctuation, space, newline, and tab.
It does not support Unicode, IME input, emoji, dead keys, or non-US layout
correction.

## Return Values

Most operations return `true` on success. Runtime failures return `nil, err`.
Argument validation errors raise Lua errors.

Typical failures are:
- `HID not initialized`: call `ble_hid.init()` first
- `not connected`: pair and connect from the host before sending reports
- `unsupported ...`: use one of the supported key, modifier, media, button, or gesture names

## HID Reports

The module owns one BLE HID service with three input reports:
- Consumer Control: report ID `1`, 1 byte
- Keyboard: report ID `2`, 8 bytes, `modifier + reserved + 6 keycodes`
- Mouse: report ID `3`, 5 bytes, `buttons + x + y + wheel + horizontal pan`

The C implementation sends report payload bytes with:

```c
esp_hidd_dev_input_set(dev, 0, report_id, data, len);
```

The payload does not include the report ID. ESP-IDF `esp_hidd` owns the HID GATT
service, characteristics, CCCD handling, protocol mode, control point, and BLE
notification transport.
