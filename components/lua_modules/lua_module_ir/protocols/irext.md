# IRext air-conditioner codesets

## Scope

The SYSTEM image contains 58 experimental codesets for Gree, Midea, Haier, Hisense, Daikin, Mitsubishi Electric, Panasonic, LG, Chigo, Kelon, Changhong, Galanz, and Chunlan, at most five per brand. Each one is a `<brand>.irext_<remote>` line in `fs_image/ir/protocols.jsonl` and is tied to one IRext `remote_index` row and binary. The database does not provide appliance model numbers, market coverage, or device verification, so these entries do not claim brand-wide compatibility.

Every codeset is an inline, encode-only JSON protocol: a payload template, disjoint bit fields with raw value tables, and sum checksums, wrapped in the IRext timing steps. Messages are selected by the values they imply:

- a generic state frame;
- per-mode frames (`implied.mode`) when a mode encodes differently, e.g. no temperature in fan mode;
- solo-key frames (`implied.key`) when a function key overwrites state bits, e.g. the Hisense 11677 power key;
- an `off` frame (`implied.power = false`).

Exposed fields:

| Field | Codesets | Notes |
| --- | --- | --- |
| `power`, `mode`, `fan` | 58 / 56 / 57 | `mode_limits` in 42 codesets restrict fans, temperatures, swing or keys per mode |
| `temperature` | 57 | whole degrees Celsius |
| `swing_v` | 39 | `auto` and the fixed `position_N` values reachable from reset |
| `key` | 27 | `power`, `mode`, `temp_up`, `temp_down`, `fan`, `swing_v`, `swing_v_fix` (IRext tags 29/34) |

The `ir_remote` library's `set()` picks the function key from the changed field; `press(key)` sends the current state with that key, flipping the cached power first for `power`. Turbo, quiet, sleep, display, timers, and half-degree temperatures are not exposed.

Capabilities are truthful: values that encode identically are merged, and a field left with fewer than two distinct encodings is removed. Notable results:

- `panasonic.irext_25073` sends one frame for every state, so it only exposes `power` and function keys.
- `panasonic.irext_25074`: swing `auto` equals `position_1`, so `swing_v` is removed.
- `hisense.irext_11677`: the power key rewrites the fan bits; pressing power uses dedicated solo-key frames while the other keys keep the fan field.
- Swing keys are only exposed with NORMAL swing data (tag 46) or their own function code. libirdecode ignores the swing patches otherwise and sends the bare template, which in `panasonic.irext_25073` is the power frame.

## Pinned source

- IRext core commit: `ebf8360d94767b17a1f0ea91366b071ac18d71e2`
- Decoder version: `1.6.0`
- IRext database commit: `fb3b471d73e118463cf609e096188f837fa8d684`
- SQLite SHA-256: `fc9507bb6a0bd65fb7d407b7a65287e66ccaa00f80067ae9cc7a425cdfaca571`
- Binary archive SHA-256: `2dccd7c5fdc09d68eda10e864f1dc9da16c6b0e79402b665440f3edd39e89f79`
- License: MIT; see [`LICENSE.IRext`](LICENSE.IRext)

The exact rows and binary digests are stored in `irext_profiles.json` next to this document.

## Selection

The generator independently rebuilds the selection from the pinned SQLite database and ZIP:

1. active `new_ac` air-conditioner rows for the thirteen fixed brands;
2. binary exists and matches the database MD5;
3. exclude generic `remote=2807`;
4. exclude binaries associated with more than three active brands, except for Chigo, whose highest-priority rows are such shared codes;
5. deduplicate binary MD5 globally, in the brand order of `BRAND_SELECTION`;
6. sort by ascending database `priority`, then row ID, and take five per brand.

`irext_profiles.json` pins an ordered subset of this selection.

Database priority is only an internal ordering value. It is not treated as sales rank, compatibility probability, or an official recommendation.

## Generation and validation

IRext is a development-time source only. The firmware contains the shared table-driven encoder and the generated JSON codesets; it does not contain the upstream decoder, SQLite database, archive, or raw binaries.

```bash
python3 components/lua_modules/lua_module_ir/protocols/irext_generate.py \
  --core-repo /path/to/irext-core \
  --database-repo /path/to/irext-database \
  --protocol-bundle components/lua_modules/lua_module_ir/fs_image/ir/protocols.jsonl
```

The generator replaces only the `*.irext_*` lines of the bundle, keeps the other codesets, and writes one minified codeset per line sorted by id. It requires clean pinned checkouts, validates every source artifact and database mapping, and parses the IRext tag format. It treats the pinned decoder as a black-box encoder and infers each field from XOR masks over the legal states. A codeset is rejected unless it splits into template, disjoint fields and checksums. Before writing a codeset it checks:

- the inferred model matches libirdecode pulse for pulse for every legal state, the off frame, and every swing position (reached by pressing swing-fix k times from reset);
- re-encoding the emitted JSON matches the model for every state it accepts;
- every value combination allowed by `limits`/`mode_limits`, and the defaults, encodes.

`--oracle-output <path>` also writes a Lua oracle of 2,076 libirdecode vectors (mode bases plus one-field variations, as pulse count and FNV-1a hash) for replaying the codesets through `ir_protocol_encode()`, the path `protocol:encode()` uses. The oracle is generated on demand and is not stored in the repository.

Validation is software-only. No target appliance has been tested, and the local Xiaomi remote-app captures are not used as proof that any IRext codeset controls a particular appliance.
