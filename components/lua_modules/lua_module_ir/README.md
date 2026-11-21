# Lua IR

The `ir` module sends and receives infrared remote-control signals through the RMT peripheral, and converts between protocol fields (a NEC address and command, an air-conditioner state, ...) and raw IR waveforms. The firmware registers it when `CONFIG_APP_CLAW_LUA_MODULE_IR` is enabled.

```lua
local ir = require("ir")
```

| API | Purpose |
| --- | --- |
| `ir.device(config)` | Open an IR transmitter and/or receiver on GPIOs |
| `ir.frame(config)` | Create a raw waveform |
| `ir.protocol.list([filter])` | List the available protocols |
| `ir.protocol.get(id)` | Open a protocol object that encodes and decodes frames |
| `ir.protocol.register(description)` | Register an encode-only protocol in the current Lua state |
| `ir.protocol.none` | The JSON `null` value inside a protocol description |

Only `ir.device` owns hardware. Frames and protocol objects are plain data and need no cleanup.

The component also provides the pure-Lua library `ir_remote`: a remote control with mapped keys, air-conditioner state, learned keys, and snapshots that can be saved as JSON. It is documented in [`lib/ir_remote.md`](lib/ir_remote.md), which the firmware ships as `/system/scripts/builtin/lib/ir_remote.md`.

```lua
local ir_remote = require("ir_remote")
local tv = ir_remote.new({protocol = "nec", fixed = {address = 0x10}, keys = {power = {command = 0x20}}})
local frame = tv:press("power")
```

## Errors

Invalid arguments, unknown fields in argument tables, unsupported values, unknown protocol ids, and driver failures raise Lua errors. Messages start with the API name, for example `protocol:encode: unsupported value`.

Expected outcomes of receiving and decoding return `nil, reason` instead:

| Reason | Returned by | Meaning |
| --- | --- | --- |
| `timeout` | `device:receive()`, `device:read()` | No frame arrived within `timeout_ms` |
| `empty_frame` | `device:receive()`, `device:read()` | Only glitches shorter than 100 µs were captured |
| `frame_too_long` | `device:receive()`, `device:read()` | The frame filled `rx_max_symbols`, or a pulse was longer than 10 s |
| `no_match` | `protocol:decode()` | The timing does not match the protocol |
| `checksum` | `protocol:decode()` | The timing matches but the checksum is wrong |
| `unsupported_value` | `protocol:decode()` | The bits decode to a value outside the protocol limits |
| `not_decodable` | `protocol:decode()` | The protocol is encode-only |

## Fields

All protocols share one field vocabulary. Values are Lua tables such as `{address = 0x10, command = 0x20}` or `{power = true, mode = "cool", temperature = 24, fan = "auto"}`.

| Field | Type | Values |
| --- | --- | --- |
| `address`, `command` | integer | Within the protocol bit width |
| `toggle` | boolean | RC5 toggle bit |
| `power` | boolean | |
| `mode` | enum | `auto`, `cool`, `dry`, `heat`, `fan` |
| `temperature` | number | Degrees Celsius, at most one decimal |
| `fan` | enum | `auto`, `min`, `low`, `medium`, `high`, `max` |
| `swing_v`, `swing_h` | enum | `off`, `auto`, `position_1`, `position_2`, ... up to `position_30` |
| `turbo`, `quiet`, `econo`, `light`, `sleep`, `clean`, `filter`, `beep` | boolean | |
| `key` | enum | Air-conditioner function key: `power`, `mode`, `temp_up`, `temp_down`, `fan`, `swing_v`, `swing_v_fix`, `swing_h`, `turbo`, `quiet`, `econo`, `light`, `sleep`, `clean`, `filter`, `beep` |

Each protocol uses a subset, reported by `protocol:info().fields` and `protocol:capabilities()`. Names outside this table raise `unknown field`; a wrongly typed value raises an error.

## Protocols

### Protocol ids

`ir.protocol.get(id)` looks an id up in this order and uses the first match:

1. Builtin protocols. They encode and decode:

   | id | Fields | Carrier | Checked |
   | --- | --- | --- | --- |
   | `nec` | `address` 0–0xFF, `command` 0–0xFF | 38 kHz | yes |
   | `nec.extended` | `address` 0–0xFFFF, `command` 0–0xFF | 38 kHz | yes |
   | `samsung32` | `address` 0–0xFFFF, `command` 0–0xFF | 38 kHz | yes |
   | `sony12` | `address` 0–0x1F, `command` 0–0x7F | 40 kHz | no |
   | `sony15` | `address` 0–0xFF, `command` 0–0x7F | 40 kHz | no |
   | `sony20` | `address` 0–0x1FFF, `command` 0–0x7F | 40 kHz | no |
   | `rc5` | `address` 0–0x1F, `command` 0–0x3F, `toggle` | 36 kHz | no |
   | `jvc` | `address` 0–0xFF, `command` 0–0xFF | 38 kHz | no |
   | `gree` | `power`, `mode`, `temperature`, `fan`, `swing_v` | 38 kHz | yes |
   | `haier.a6` | `power`, `mode`, `temperature`, `fan`, `swing_v`, `sleep`, `key` | 38 kHz | yes |
   | `daikin.280` | `power`, `mode`, `temperature`, `fan`, `swing_v`, `swing_h` | 38 kHz | yes |
   | `midea.coolix` | `power`, `mode`, `temperature`, `fan`, `key` | 38 kHz | yes |
   | `hisense.kelon168` | `power`, `mode`, `temperature`, `fan`, `swing_v`, `key` | 38 kHz | yes |
   | `aux.electra` | `power`, `mode`, `temperature`, `fan`, `swing_v`, `sleep`, `key` | 38 kHz | yes |
   | `tcl.112` | `power`, `mode`, `temperature`, `fan`, `swing_v`, `key` | 38 kHz | yes |
   | `xiaomi.96` | `power`, `mode`, `temperature`, `fan`, `swing_v`, `sleep` | 38 kHz | yes |

   "Checked" protocols carry a checksum or an inverted copy, so a successful decode is reliable; decode results of unchecked protocols are weaker evidence. The air-conditioner protocols carry defaults and limits, see `protocol:capabilities()`.
2. Protocols registered in the current Lua state with `ir.protocol.register()`.
3. `ir/protocols/<id>.json` under the DATA root: user protocols, one JSON file each, in the [description format](#protocol-description-format). Build the directory path with `storage.join_path(storage.get_root_dir(), "ir/protocols")`.
4. `/system/ir/protocols.jsonl`: 58 experimental air-conditioner codesets generated from IRext, with ids such as `gree.irext_10727`. Their scope and limitations are described in [`protocols/irext.md`](protocols/irext.md) in the component source.

Registered and JSON protocols are encode-only. A DATA file overrides a SYSTEM codeset with the same id; if that file is invalid, `ir.protocol.get()` raises an error instead of falling back.

Ids use lowercase letters, digits, `_`, `.`, and `-`, are 1–63 bytes long, and do not start with `.`.

### `ir.protocol.list([filter])`

Returns an array of `protocol:info()` tables: builtin protocols, then registered protocols in registration order, then JSON protocols.

```lua
for _, info in ipairs(ir.protocol.list({category = "air_conditioner", brand = "Gree"})) do
    print(info.id, info.model or "")
end
```

- `filter` accepts `category` and `brand`, matched exactly. Protocols without the attribute, such as `nec`, are left out when the filter names it.
- JSON files that fail to parse are skipped.
- The call parses every JSON protocol, so run it once, for example when a UI starts, not on every key press.

### `ir.protocol.get(id)`

Returns a protocol object, or raises `unknown protocol '<id>'`. JSON protocols are read and parsed on every call, so keep the object while you use it. Objects are freed by the garbage collector.

#### `protocol:info()`

Returns `{id, carrier_hz, decode, checked, fields}`. Protocols that carry attributes add `category`, `brand`, `model`, `source`, and `maturity`; `maturity = "experimental"` marks codesets not fully verified on real devices.

#### `protocol:capabilities([context])`

```lua
local caps = ir.protocol.get("xiaomi.96"):capabilities({mode = "dry"})
-- caps.state.temperature = {type = "number", min = 16, max = 30, step = 1, default = 26}
-- caps.state.fan = {type = "enum", values = {"low"}, default = "auto"}
-- caps.keys = {}
```

- `state` describes each state field (every field except `address`, `command`, `toggle`, and `key`): its `type` (`boolean`, `enum`, `number`, or `integer`), the allowed `values` or `min`/`max`/`step` when the protocol limits the field, and `default` when the protocol has one. Defaults do not depend on the mode. Fields the mode does not send are omitted, e.g. `temperature` in the fan mode of `daikin.280`.
- `keys` lists the function keys allowed in the mode. It is empty when the protocol has no `key` field.
- `context` only accepts `mode`. Without it, the mode-independent limits are returned.

#### `protocol:check(values)`

Returns `true` when every given value is within the protocol limits for `values.mode`. Missing fields and fields the mode does not send are not checked.

#### `protocol:encode(values[, options])`

Returns a frame that uses the protocol carrier.

```lua
local nec = ir.protocol.get("nec")
local frame = nec:encode({address = 0x10, command = 0x20})
local held = nec:encode({address = 0x10, command = 0x20}, {hold_ms = 600})
```

- Raises `missing or invalid field` when a field the message needs is absent, and `unsupported value` when a value is outside the protocol range or limits. Fields the protocol does not use are ignored. `toggle` defaults to `false`.
- A protocol may have several messages. The one whose implied values are the most specific match is used, e.g. `{power = false}` selects the off message of codesets that have one.
- `options.hold_ms` (default 0) emulates holding the key: `nec` and `nec.extended` append repeat codes, other protocols repeat the frame, as many times as fit in `hold_ms`, up to the 4096-pulse frame limit.

#### `protocol:decode(frame[, options])`

```lua
local values, info = nec:decode(frame)
-- values = {address = 0x10, command = 0x20}
-- info = {message = "frame", copies = 1, holds = 0, checked = true, next = 69}
```

- Decodes with this protocol only; there is no protocol detection. When the source is unknown, try candidate protocols in turn.
- `values` can be passed to `encode()` of the same protocol to reproduce the signal.
- `info.message` is the matched message name. `info.copies` counts the frame plus identical repeats right after it, and `info.holds` the hold messages (such as NEC repeat codes) after those.
- `info.next` is the index of the first pulse after the decoded part. When it is at most `#frame`, more pulses follow and can be decoded with `{offset = info.next}`.
- `options.offset` is the first pulse to decode, 1-based, default 1. An out-of-range offset raises an error.
- Failures return `nil, reason`, see [Errors](#errors).

### `ir.protocol.register(description)`

Registers an encode-only protocol described by a Lua table in the [description format](#protocol-description-format) and returns its object. `ir.protocol.get(id)`, `ir.protocol.list()`, and `ir_remote` then find it by id.

```lua
local tiny = ir.protocol.register({
    id = "my.tiny",
    protocol = {
        carrier_hz = 38000,
        messages = {{
            layout = {symbols = {{560, 560}, {560, 1690}}, steps = {"M9000", "S4500", "B8", "M560"}},
            length = 1,
            fields = {{field = "command", segments = {{0, 0, 8}}}},
        }},
    },
})
local frame = tiny:encode({command = 0x2A})
local remote = require("ir_remote").new({protocol = "my.tiny", keys = {ok = {command = 0x2A}}})
```

- The registration exists only in the current Lua state. Each script run and each App has its own state, so other scripts cannot see it and it is gone when the script ends. Register it again before restoring an `ir_remote` snapshot that uses it. For a permanent protocol, save the same content as a DATA JSON file.
- The id must not be a builtin id or one already registered in this state. It may shadow a JSON protocol with the same id.
- JSON mapping: objects are string-keyed tables, arrays are 1..n sequences, and `null` is `ir.protocol.none`, e.g. `raw = {0, ir.protocol.none, 21}`. An empty table is omitted as an object member and becomes `{}` as an array element. Integers are passed exactly. Tables nest at most 16 levels.
- An invalid description raises an error and registers nothing.

### Protocol description format

DATA JSON files and `ir.protocol.register()` use the same format. This excerpt from `gree.irext_10727` replaces omitted parts with `"..."`:

```json
{
  "id": "gree.irext_10727",
  "category": "air_conditioner",
  "brand": "Gree",
  "model": "IRext 10727",
  "protocol": {
    "carrier_hz": 38000,
    "messages": [
      {
        "name": "frame",
        "implied": {"power": true},
        "layout": {"symbols": [[610, 580], [610, 1680]], "steps": ["M9000", "S4500", "B35", "M610", "S20000", "B32", "M610", "S40000", "..."]},
        "length": 18,
        "widths": [[4, 3], [13, 3]],
        "template": "490020500211200050490020700200000030",
        "fields": [{"field": "temperature", "segments": [[1, 0, 4], [10, 0, 4]], "raw": [0, 17, 34, "..."], "base": 160, "step": 10}, "..."],
        "checks": [{"op": "sum", "target": 16, "units": [1, 3, 5, 7, 10], "nibble": true, "bias": 12}]
      },
      {"name": "off", "implied": {"power": false}, "...": "..."}
    ]
  },
  "defaults": {"power": true, "mode": "cool", "temperature": 26, "fan": "auto"},
  "limits": {"mode": ["auto", "cool", "dry", "heat", "fan"], "temperature": {"min": 16, "max": 30, "step": 1}, "fan": ["auto", "low", "medium", "high"]},
  "mode_limits": {"dry": {"fan": ["low"]}},
  "source": "irext:10727 (remote_index 3581, irext core 1.6.0)",
  "maturity": "experimental"
}
```

Top-level keys; any other key is rejected:

| Key | Required | Meaning |
| --- | --- | --- |
| `id` | yes | Protocol id. A DATA file must be named `<id>.json` |
| `protocol` | yes | Carrier and messages, see below |
| `category`, `brand`, `model`, `source`, `maturity` | no | Strings reported by `info()`; `category` and `brand` are used by `list()` filters |
| `defaults` | no | Initial state. Only fields the protocol uses, within the limits |
| `limits` | no | Allowed values: an array for enum and boolean fields, `{min, max, step}` for numbers |
| `mode_limits` | no | Per-mode overrides of `limits`, keyed by mode. `null` marks a field the mode does not send, e.g. `{"fan": {"temperature": null}}` (`ir.protocol.none` in Lua) |

`protocol` contains `carrier_hz` (1000–1,000,000), a non-empty `messages` array, and an optional `hold` message that `hold_ms` appends instead of repeating the selected message. A message has these keys:

| Key | Meaning |
| --- | --- |
| `name` | Reported by `decode()`; default `"frame"` |
| `implied` | Values that select this message, e.g. `{"power": false}` |
| `layout` | Timing, see below |
| `length` | Payload length in bytes, 0–48 |
| `template` | Hex string of `length` bytes; fields and checks are written over it |
| `widths` | `[[byte, bits], ...]` for bytes that send fewer than 8 bits |
| `msb_first` | Send each byte most significant bit first; default least significant bit first |
| `copies` | Send the message this many times, 1–255; default 1 |
| `fields` | Where field values go, see below |
| `checks` | Checksums, applied in order after the fields |

- `layout.steps` is an array of `"M<us>"` (mark), `"S<us>"` (space), `"P<us>"` (space until the message has lasted `<us>` since it started), and `"B<n>"` (the next `n` payload bits, at most 384).
- `layout.coding` is `"pair"` (default) or `"biphase"`. Pair coding sends bit 0 and bit 1 as `symbols = [[mark, space], [mark, space]]`. With four symbols each one carries two bits: `symbols[v + 1]` sends the value `v` of the next two bits, the first sent bit being the high one, and every `"B<n>"` must have an even `n`. Decoding picks the nearest symbol, so each pair of symbols needs a mark or a space that lies outside the tolerance window of the other. Biphase (Manchester) coding uses `half_us` and `one_level`, the level of the first half of a 1 bit (default 1).
- A field is `{"field": name, "segments": [[byte, shift, width], ...], "base": 0, "step": 1, "raw": [...]}`. The segments hold the value starting from its least significant bits, in order; `shift` is 0–7 and `width` 1–8. A value becomes the index `(value - base) / step`; temperatures count in 0.1 °C (`base` 160 is 16 °C, `step` 10 is 1 °C), enums by their position in the [field table](#fields) from 0, and booleans as 0 and 1. With `raw`, the index selects the raw code and `null` marks an unsupported value. Without `raw`, the index itself is written and must fit in the segments.
- A check is `{"op": "sum" | "xor" | "copy", "target": unit, "units": [...]}`, or uses `"first"` and `"count"` instead of `units`. `sum` adds the source units and `bias`, `xor` XORs them, and both write the result, truncated to one unit, to `target`. `copy` copies the source units to `target`, `target + 1`, and so on. Units are bytes, or nibbles when `nibble` is `true` (unit `2k` is the high nibble of byte `k`); `byte_target` keeps the target in bytes. `invert` inverts the written value.
- Every array of a message (`steps`, `symbols`, `widths`, `fields`, `segments`, `raw`, `checks`, `units`) and `messages` itself hold at most 255 items; longer arrays raise `too large`.

[`protocols/`](protocols/) in the component source records where the builtin air-conditioner tables come from and how the IRext codesets are generated; it is not part of the firmware image.

## Frames

### `ir.frame(config)`

```lua
local frame = ir.frame({carrier_hz = 38000, pulses_us = {9000, 4500, 560, 560, 560}})
```

- `pulses_us`: 1–4096 integers of 1–10,000,000 µs, alternating mark, space, mark, ... and starting with a mark.
- `carrier_hz`: 0 or 1000–1,000,000, default 0. A frame with carrier 0 is sent with the device carrier.

Frames are immutable. `#frame` returns the pulse count, `frame:info()` returns `{carrier_hz, pulse_count, duration_us}`, and `frame:to_table()` returns a new `{carrier_hz, pulses_us}` table that `ir.frame()` accepts again and that can be stored as JSON.

Received frames have `carrier_hz = 0`, with glitches shorter than 100 µs removed.

## Devices

### `ir.device(config)`

```lua
-- ESP32-S3-BOX-3 IR pins.
local device = ir.device({tx_gpio = 39, rx_gpio = 38, ctrl_gpio = 44, ctrl_active_level = 0})
```

At least one of `tx_gpio` and `rx_gpio` is required:

| Key | Default | Meaning |
| --- | --- | --- |
| `name` | `"ir"` | Label, at most 63 bytes |
| `tx_gpio` | `-1` | Output to the IR LED driver; `-1` disables TX |
| `rx_gpio` | `-1` | Input from an IR receiver module; `-1` disables RX |
| `ctrl_gpio` | `-1` | Optional enable output of the IR circuit |
| `ctrl_active_level` | `0` | Level that enables `ctrl_gpio`, 0 or 1 |
| `tx_invert` | `false` | Invert the TX output |
| `rx_invert` | `true` | Invert the RX input; receiver modules output low during a mark |
| `carrier_hz` | 38000 | Carrier for frames with carrier 0, 1000–1,000,000 |
| `carrier_duty` | `0.33` | Carrier duty cycle, greater than 0 and less than 1 |
| `tx_resolution_hz` | 1,000,000 | RMT TX tick rate |
| `rx_resolution_hz` | 500,000 | RMT RX tick rate |
| `rx_max_symbols` | 512 | Receive buffer in RMT symbols (one mark and one space each), 2–2048 |
| `signal_range_min_ns` | 1250 | Receive default: shorter pulses are filtered as glitches |
| `signal_range_max_ns` | 60,000,000 | Receive default: a space this long ends the frame |

- `carrier_hz`, the resolutions, and `rx_max_symbols` default to the Kconfig values below.
- Unknown keys raise `ir.device: unknown field`. TX, RX, and ctrl must be different GPIOs that support their direction, otherwise `ir.device: invalid configuration` is raised.
- The GPIOs are claimed when the device is created. A GPIO already claimed by another device or driver raises `ir.device: resource claim failed`.
- `signal_range_max_ns × rx_resolution_hz` must fit in the RMT idle counter (32767 ticks on chips such as the ESP32-S3); the default 60 ms at 500 kHz fits and covers the 40 ms gaps of long air-conditioner frames. The hardware glitch filter also caps `signal_range_min_ns` at a few microseconds. Values beyond these limits fail when receiving starts.

### `device:send(frame[, options])`

Transmits the frame once and blocks until it is finished; returns `true`.

- `options.timeout_ms` (default 5000) bounds the wait.
- The frame carrier is used, or the device `carrier_hz` when the frame carrier is 0.
- Raises `ir: TX GPIO not configured`, `ir: transmit wait failed: ESP_ERR_TIMEOUT` after a timeout (TX is reset), or `ir: transmit failed: <error>`, for example when no RMT channel is free.

### `device:receive([options])`

Starts the receiver and blocks until one frame arrives or the timeout expires. Returns a frame, or `nil, reason` with `timeout`, `empty_frame`, or `frame_too_long`. The receiver is stopped afterwards in every case.

- `options.timeout_ms` (default 5000) bounds the wait.
- `options.signal_range_min_ns` and `options.signal_range_max_ns` override the device values for this call.
- Raises `ir: RX GPIO not configured`, or `ir: RX start failed: <error>`, for example while a `start_receive()` capture is pending.

### `device:start_receive([options])`, `device:read([options])`, `device:stop_receive()`

Split reception arms the receiver before doing something else, for example sending a frame in an optical loopback test:

```lua
device:start_receive()
device:send(frame)
local received, reason = device:read({timeout_ms = 1000})
device:stop_receive()
```

- `start_receive()` accepts `signal_range_min_ns` and `signal_range_max_ns`, arms one capture, and returns `true` immediately. It raises when a capture is already pending.
- `read()` accepts `timeout_ms` (default 5000; 0 checks without waiting) and returns like `receive()`. It raises `ir: receiver is not started` when no capture is pending. After `timeout` the capture stays pending and `read()` can be called again; any other result ends it, so call `start_receive()` again for the next frame.
- `stop_receive()` cancels a pending capture and returns `true`, also when nothing is pending.

### `device:info()`

Returns `{name, tx_gpio, rx_gpio, ctrl_gpio, carrier_hz, tx_resolution_hz, rx_resolution_hz, rx_max_symbols, tx_invert, rx_invert}`.

### `device:close()`

Releases the RMT channels, the ctrl GPIO, and the GPIO claims. Calling it again does nothing; any other method on a closed device raises `ir: invalid or closed device`. If `close()` raises, retry it before reusing the GPIOs.

Close the device when done, also on error paths. The garbage collector and job shutdown close a forgotten device, but its GPIOs stay claimed until then.

```lua
local device = ir.device({tx_gpio = 39})
local ok, err = pcall(device.send, device, ir.protocol.get("nec"):encode({address = 0x10, command = 0x20}))
device:close()
assert(ok, err)
```

### Resources, blocking, and concurrency

- A device uses at most one RMT TX channel and one RMT RX channel, allocated by the first send and the first receive and kept until `close()`. Other drivers, such as LED strips, use the same RMT channels; when none is free, sending or receiving raises an error.
- The RX channel comes with a receive buffer of `rx_max_symbols × 4` bytes. Each send allocates a temporary TX buffer.
- `ctrl_gpio` is driven to `ctrl_active_level` when the first RMT channel is allocated, and to the inactive level and reset by `close()`.
- `send()`, `receive()`, and `read()` block the calling Lua task up to their timeouts; other scripts keep running. Use bounded timeouts in loops so the script can be stopped promptly.
- Sending does not pause a pending capture, so a receiver on the same board sees its own transmission.
- A device belongs to the Lua state that created it. Two devices, in the same or different scripts, cannot share a GPIO; the second `ir.device()` fails until the first is closed.

### Wiring and safe GPIO use

- Drive the IR LED through a transistor or LED driver with a current-limiting resistor; do not power a high-current IR LED directly from a GPIO.
- Connect `rx_gpio` to the output of a demodulating IR receiver module that matches the carrier, such as a 38 kHz receiver. Its active-low output matches the default `rx_invert = true`; set `rx_invert = false` for an active-high front end.
- Set `ctrl_gpio` and `ctrl_active_level` only when the board has an enable line for its IR circuit, using the board schematic.
- Use free GPIOs suitable for the connected hardware. Avoid strapping pins, flash and PSRAM pins, USB pins, and the console UART pins.

## Kconfig

| Option | Default | Meaning |
| --- | --- | --- |
| `CONFIG_APP_CLAW_LUA_MODULE_IR` | `n` | Build and register the module |
| `CONFIG_LUA_MODULE_IR_DEFAULT_CARRIER_HZ` | 38000 (30000–56000) | Default `carrier_hz` |
| `CONFIG_LUA_MODULE_IR_DEFAULT_TX_RESOLUTION_HZ` | 1000000 | Default `tx_resolution_hz` |
| `CONFIG_LUA_MODULE_IR_DEFAULT_RX_RESOLUTION_HZ` | 500000 | Default `rx_resolution_hz` |
| `CONFIG_LUA_MODULE_IR_RX_MAX_SYMBOLS` | 512 (32–2048) | Default `rx_max_symbols`; raise it to capture long frames in one shot |

## Example

Send an air-conditioner state, then learn a key from a TV remote, try to decode it as NEC, and replay it:

```lua
local ir = require("ir")

local device = ir.device({tx_gpio = 39, rx_gpio = 38, ctrl_gpio = 44})
local ok, err = pcall(function()
    local ac = ir.protocol.get("gree.irext_10727")
    device:send(ac:encode({power = true, mode = "cool", temperature = 24, fan = "auto", swing_v = "auto"}))

    local frame, reason = device:receive({timeout_ms = 5000})
    if not frame then
        print("receive: " .. reason)
        return
    end
    local values = ir.protocol.get("nec"):decode(frame)
    print(values and string.format("nec address=0x%02X command=0x%02X", values.address, values.command) or "not NEC")
    device:send(frame)
end)
device:close()
assert(ok, err)
```

## Tests

The scripts in `test/` are bundled with the `builtin_lua_modules` skill. `ir_codec.lua`, `ir_json_protocols.lua`, `ir_protocol_register.lua`, and `ir_remote_lib.lua` need no hardware. The following need a board with IR TX and RX; they default to the ESP32-S3-BOX-3 pins, which `args` can override:

- `ir_device_smoke.lua`: device API contract.
- `ir_learn_replay.lua`: learns a key from a remote within 5 s and replays it.
- `ir_loopback.lua`: TX→RX loopback on one board. With `args.protocols = true` it encodes, sends, receives, and decodes every builtin protocol, which emits real remote codes.

## Licenses

The license of the IRext codesets is in [`protocols/LICENSE.IRext`](protocols/LICENSE.IRext) in the component source.
