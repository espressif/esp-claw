---
{
  "name": "ble_hid",
  "description": "Operate ESP-Claw BLE HID: start HID advertising and send media/keyboard/mouse actions.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# BLE HID

Use this skill whenever the user asks to start BLE HID, make the device show up
as a keyboard/mouse/media-control device, send media controls, type keys/text,
or move/click/scroll the mouse.

## Start BLE HID

```json
{"path":"{CUR_SKILL_DIR}/scripts/start_ble_hid.lua","args":{"name":"esp-claw-hid"},"timeout_ms":10000}
```

BLE HID is a composite device. Media, keyboard, and mouse reports are all
available at the same time; the action `type` decides which report is sent.

## Send One Action

Media:

```json
{"path":"{CUR_SKILL_DIR}/scripts/send_ble_hid_action.lua","args":{"type":"media","key":"play_pause"},"timeout_ms":5000}
```

Keyboard key:

```json
{"path":"{CUR_SKILL_DIR}/scripts/send_ble_hid_action.lua","args":{"type":"keyboard_key","key":"SPACE"},"timeout_ms":5000}
```

Keyboard combo:

```json
{"path":"{CUR_SKILL_DIR}/scripts/send_ble_hid_action.lua","args":{"type":"keyboard_combo","keys":["CTRL","C"]},"timeout_ms":5000}
```

Text:

```json
{"path":"{CUR_SKILL_DIR}/scripts/send_ble_hid_action.lua","args":{"type":"keyboard_text","text":"hello"},"timeout_ms":5000}
```

Mouse:

```json
{"path":"{CUR_SKILL_DIR}/scripts/send_ble_hid_action.lua","args":{"type":"mouse_move","dx":30,"dy":0},"timeout_ms":5000}
```
