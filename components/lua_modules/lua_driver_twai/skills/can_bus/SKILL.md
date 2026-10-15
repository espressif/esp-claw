---
{
  "name": "can_bus",
  "description": "Use ESP32 TWAI/Classical CAN to send and receive CAN frames, monitor a CAN bus, or show live CAN TX/RX data on the LCD. Requires explicit GPIOs and board_hardware_info before assigning pins.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# CAN Bus

Use this skill for ESP32 TWAI (Classical CAN) requests. ESP32-S3 does not
support CAN FD frames, although a CAN-FD-capable transceiver can carry
Classical CAN traffic.

Read `board_hardware_info` before assigning GPIOs. Use the user's explicit
pins only after confirming they do not conflict with board hardware.

For continuous transmit, receive, and LCD monitoring, run exactly one bundled
script asynchronously. If execution returns an error, report it directly; do
not retry with different pins or bit rate unless the user asks.

## Monitor Script Args

```json
{
  "type": "object",
  "required": ["tx_gpio", "rx_gpio"],
  "properties": {
    "tx_gpio": {
      "type": "integer",
      "minimum": 0
    },
    "rx_gpio": {
      "type": "integer",
      "minimum": 0
    },
    "bitrate": {
      "type": "integer",
      "enum": [25000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000],
      "default": 500000
    },
    "mode": {
      "type": "string",
      "enum": ["normal", "no_ack", "listen_only"],
      "default": "normal"
    },
    "id": {
      "type": "integer",
      "minimum": 0,
      "maximum": 536870911,
      "default": 291
    },
    "extended": {
      "type": "boolean",
      "default": false
    },
    "single_shot": {
      "type": "boolean",
      "default": true
    },
    "interval_ms": {
      "type": "integer",
      "minimum": 10,
      "default": 1000
    },
    "initial_value": {
      "type": "integer",
      "minimum": 0,
      "maximum": 2147483647,
      "default": 1
    }
  }
}
```

The transmitted payload is always eight bytes. The first four bytes are zero;
the final four bytes contain `initial_value` as a big-endian counter. The Lua
seed is limited to `0x7FFFFFFF` because this firmware uses 32-bit signed Lua
integers. After initialization, byte-wise carry preserves the full unsigned
32-bit counter and wraps to zero after `0xFFFFFFFF`.

The monitor uses single-shot transmission by default. In Normal mode this
prevents one frame without an ACK from being retried forever and filling the
transmit queue; controller error counters still reveal a missing ACK. Set
`single_shot` to `false` only when automatic CAN retransmission is required.

The LCD centers a compact `TX:<hex data>` line and `RX:<hex data>` line. It
shows the final four payload bytes (or the complete payload when shorter) so
the counter fits without clipping, for example `TX:0000000A`.

## Tool Call

Use `lua_run_script_async` with:

- Path: `{CUR_SKILL_DIR}/scripts/can_lcd_monitor.lua`
- Timeout: `0`
- Name: `can_lcd_monitor`
- Exclusive: `display_app`
- Replace: `true`

For TD501MCANFD on the LilyGo T-Connect-Pro S3 described by the user:

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/can_lcd_monitor.lua",
  "args": {
    "tx_gpio": 6,
    "rx_gpio": 7,
    "bitrate": 500000,
    "mode": "normal",
    "id": 291,
    "extended": false,
    "single_shot": true,
    "interval_ms": 1000,
    "initial_value": 1
  },
  "timeout_ms": 0,
  "name": "can_lcd_monitor",
  "exclusive": "display_app",
  "replace": true
}
```

Stop the async Lua job to release TWAI and the display. The script also closes
both resources when an ordinary runtime error occurs.

## Electrical Checks

- Connect ESP `TX GPIO` to the transceiver logic TX input and ESP `RX GPIO` to
  the transceiver logic RX output. Do not connect ESP GPIO directly to CANH or
  CANL.
- Confirm the transceiver module's logic-side supply and RX output are 3.3 V
  compatible, and connect the required logic-side ground.
- Put 120-ohm termination at both physical ends of CANH/CANL.
- Normal mode requires at least one other active node to acknowledge frames.
  Without an ACK the controller eventually enters bus-off; the monitor attempts
  recovery but wiring or topology must still be corrected.
