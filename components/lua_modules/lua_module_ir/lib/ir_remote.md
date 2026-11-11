# ir_remote.lua

Pure-Lua IR remote built on the `ir` module. A remote holds learned keys, mapped keys, AC state and the toggle bit, and saves everything as a plain table snapshot. It only builds and decodes frames; sending and receiving stay with `ir.device()`.

## Require

```lua
local ir_remote = require("ir_remote")
```

The script's own directory is searched first, so do not name a script `ir_remote.lua`.

## Functions

### `ir_remote.new([config])`

Creates a remote. Errors are raised as Lua errors that start with `ir_remote:` or carry the `ir` message.

```lua
-- AC protocol: a builtin table or a codeset
local ac = ir_remote.new({protocol = "gree.irext_10727"})
-- Fixed fields and mapped keys
local tv = ir_remote.new({protocol = "nec", fixed = {address = 0x10}, keys = {power = {command = 0x20}, volume_up = {command = 0x21}}})
-- Learning only
local custom = ir_remote.new()
-- Restored from a snapshot
local restored = ir_remote.new(tv:to_table())
```

All config fields are optional; unknown fields raise an error.

- `protocol`: any id accepted by `ir.protocol.get()`.
- `fixed`: fields sent with every mapped key, e.g. the device address. Needs `protocol`; every field must belong to the protocol and be within its limits.
- `keys`: `{name = {field = value}}` mapped keys. Needs `protocol`; invalid key values raise an error here.
- `state`: initial AC state, merged over the protocol defaults. Needs `protocol`; fields must be state fields within the protocol limits.
- `learned`: `{name = {carrier_hz, pulses_us}}` frames in the `frame:to_table()` format.
- `match_raw`: when `true`, `decode()` also matches learned frames. Default `false`.
- `version`: snapshot version; only `1` is accepted.

Key names are strings of 1–32 bytes.

## Methods

### `remote:press(key[, options])`

Returns the frame for a key, looked up in this order:

1. Learned keys: the stored frame.
2. Mapped keys: `fixed` merged with the key values.
3. Function keys of the current mode, for protocols with a `key` field: the current state encoded with that key. `power` also flips the cached power, like `set({power = not power, key = "power"})`.

`options.hold_ms` appends hold messages as in `protocol:encode()`; learned keys ignore it. On protocols with a `toggle` field, every press of a mapped key flips the toggle bit. Unknown keys raise an error.

### `remote:set(changes)`

Changes the AC state and returns the full state frame. Needs a protocol.

```lua
local frame = ac:set({mode = "cool", temperature = 24})
```

1. `changes` is merged into the current state. Fields that are not state fields, or that the new mode does not send (absent from `capabilities({mode = ...}).state`), raise `unsupported value`. Values of fields the new mode does not send are kept for later modes.
2. Unchanged fields that the new mode does not allow move to an allowed value: the first listed value, or the value clamped to the range and rounded down to a step. For example, entering dry mode on `xiaomi.96` sets the fan to low.
3. On protocols with a `key` field, a function key is chosen from the change, e.g. `temp_up` when the temperature rises, or `swing_v_fix` for a fixed vane position (`swing_v` when the protocol has no `swing_v_fix`). Keys the mode does not allow are skipped. Without a changed field, the first allowed key is used, skipping `power` while power is unchanged. `changes.key` selects the key explicitly; on protocols without a `key` field it raises an error.
4. The state is encoded; on success it becomes the current state. Values outside the protocol limits raise `unsupported value` and leave the state unchanged.

`set({power = false})` returns the power-off frame of the protocol.

### `remote:state()`

Returns a copy of the current AC state, e.g. `{power = true, mode = "cool", temperature = 24, fan = "auto"}`. It is empty for protocols without state fields.

### `remote:capabilities([context])`

Same as `protocol:capabilities()`. Returns `{state = {}, keys = {}}` without a protocol.

### `remote:keys()`

Returns `{name = source}`, where source is `learned`, `mapped` or `function` (function keys of the current mode).

### `remote:learn(key, frame)`

Stores an `ir.frame` under `key`, replacing a learned key with the same name. The frame is kept as received and not decoded, so replay does not depend on protocol support. Frames from `device:receive()` have carrier 0 and are replayed with the device carrier. A remote holds at most 64 learned keys and 16,384 learned pulses; exceeding either raises an error.

### `remote:forget(key)`

Removes a learned key and returns `true` when it existed. A mapped or function key with the same name becomes reachable again.

### `remote:decode(frame)`

```lua
local event, reason = tv:decode(received)
-- {key = "volume_up"} or {state = {mode = "heat", temperature = 28}}
```

1. With `match_raw`, a learned frame matches when the pulse counts are equal and every pulse is within 25 % or 100 µs, whichever is larger.
2. The remote protocol decodes the frame within its limits; a mapped key whose values match, ignoring `toggle`, gives `{key = name}`.
3. Otherwise the decoded state fields give `{state = ...}` and update `remote:state()`.

Without a match it returns `nil, reason`: `no_match` when the remote has no protocol or nothing matched, otherwise the `protocol:decode()` reason.

### `remote:info()`

Returns `protocol:info()` of the remote protocol, or `{}` without a protocol.

### `remote:to_table()`

Returns a version 1 snapshot `{version, protocol, fixed, keys, state, match_raw, learned}` that `ir_remote.new()` accepts. `learned` is always present, and `keys` is present when there is a protocol; `protocol`, `fixed`, `state` and `match_raw` are left out when unset or empty. Learned keys use the `frame:to_table()` format. The snapshot is independent of the remote and can be stored as JSON.

A protocol from `ir.protocol.register()` exists only in the Lua state that registered it, so register it again before restoring a snapshot that uses it.

## Example

```lua
local ir = require("ir")
local ir_remote = require("ir_remote")

local device = ir.device({tx_gpio = 39, rx_gpio = 38, ctrl_gpio = 44})
local ac = ir_remote.new({protocol = "xiaomi.96"})
device:send(ac:set({power = true, mode = "dry"}))
print(ac:state().fan) -- "low", fitted to dry mode

local tv = ir_remote.new({protocol = "nec", fixed = {address = 0x10}, keys = {power = {command = 0x20}}})
device:send(tv:press("power"))
local frame = device:receive({timeout_ms = 5000})
if frame then tv:learn("netflix", frame) end
local snapshot = tv:to_table()
device:close()
```
