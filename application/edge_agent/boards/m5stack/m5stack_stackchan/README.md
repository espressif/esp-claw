# M5Stack StackChan

StackChan is M5Stack's desktop robot built around a **CoreS3** as the controller, plus a stack of add-on boards (Power, Ring, Touch, Adapter). This board definition starts from `m5stack/m5stack_cores3` and adds the StackChan-specific hardware.

| Item | Value |
|------|-------|
| Chip | ESP32-S3 |
| Controller | M5Stack CoreS3 |
| Display | ILI9341 320x240 SPI + FT5x06 touch |
| Audio | AW88298 speaker DAC + ES7210 mic ADC |
| Power | AXP2101 PMU + AW9523B IO expander (CoreS3) |
| Head motion | 2x Feetech SCS serial-bus servos |
| Add-on IO | PY32 I2C IO expander ("IOE1") |
| Power metering | INA226 |
| Light sensor | LTR-553ALS (on the CoreS3 itself) |

## What this board adds over `m5stack_cores3`

### IOE1: PY32 IO expander

A second IO expander, distinct from the CoreS3's own AW9523B, on the same internal I2C bus at 7-bit address `0x6F` (`ADD_SEL` tied low). 14 GPIOs, 4 ADC channels, 4 PWM channels and an addressable-LED RAM block.

| Pin | Function |
|-----|----------|
| 0 | `VM_EN` — servo power rail (TPS61088 boost enable) |
| 13 | `RGB` — data line for the 12 Ring LEDs |

It is declared as a `custom` device (`ioe1.c` / `ioe1.h`) rather than `type: gpio_expander` for two reasons: the PY32 boots slower than the ESP32-S3 and needs a retry window before it answers, and its ADC / PWM / LED blocks cannot be expressed through the `esp_io_expander` API.

The C device does only what has to happen before any script runs: poll the version register until the PY32 is up (`boot_timeout_ms`, default 1200 ms — the same budget the M5Stack reference BSP uses), then drive `VM_EN` high so the servo bus is usable as soon as the agent runs a script. Everything else is reachable from Lua via `lib_py32_ioe`.

### Servos

Two Feetech SCS-series serial-bus servos share one UART:

| Signal | GPIO |
|--------|------|
| `Servo_TX` | 6 |
| `Servo_RX` | 7 |

UART port `1`, 1 Mbps, Feetech SCSCL packet protocol. Servo id `1` is yaw, id `2` is pitch.

This UART is **not** declared in `board_peripherals.yaml`: a UART port has a single owner, and here the owner is Lua. Scripts open it with `uart.new(1, 6, 7, 1000000)` and drive it through `lib_scs_servo`.

Reference values from the M5Stack BSP, for anyone building a head-motion layer on top of the raw driver:

| Axis | Id | BSP default zero | BSP raw position limits | BSP angle limits (0.1 deg) |
|------|----|------------------|-------------------------|----------------------------|
| Yaw | 1 | 460 | 0..1000 | -1280 .. +1280 |
| Pitch | 2 | 620 | 0..1000 | 0 .. +900 |

One position count is 0.3125 degrees, so `raw = zero + angle_deci_degrees * 16 / 50`. The BSP zero points are **not** constants: they are per-unit mechanical calibration values, which the BSP stores in NVS and re-derives from a homing routine. They are listed here as starting values only.

### Measured travel on one unit

Hand-measured with `/system/scripts/stackchan_servo_range.lua` (torque off, head moved by hand) on 2026-08-26:

| Axis | Id | Observed counts | Span | Interpretation |
|------|----|-----------------|------|----------------|
| Yaw | 1 | 0..1022 | 319.4° | **Encoder saturated at both ends.** The axis is mechanically free to turn further than position mode can address; use PWM mode for continuous rotation. No travel limit is needed — the driver's default `0..1023` fallback already covers the whole addressable range. |
| Pitch | 2 | 576..862 | 89.4° | Both ends are real mechanical stops, matching the 90° design figure. A limit is worth setting: `bus:set_pos_limits(2, 584, 854)` leaves an 8-count margin off each stop. |

A second unit swept `581..863` (88.1°), so the stops move by a few counts between units. Anything that hard-codes one unit's numbers as a bound will reject the other unit's own calibration — see `stackchan_head`'s axis envelope for how that is handled.

Directions, confirmed on hardware 2026-08-28: **increasing counts is up on pitch and left on yaw** (from the robot's point of view).

The servo bus reads **5.1–5.2 V** when `VM_EN` is up. That is normal for this board, not a sagging rail.

These numbers describe that one unit as assembled at that moment. Re-measure after re-assembly or a servo swap. Note also that a **resting position is not a zero point** — the same unit read pitch 683 in one session and 584 in another, because rest is wherever gravity and friction left the head. A zero must come from the measured range or a homing routine, not from a single `read_pos`.

Yaw has a constraint the encoder does not express: the servo bus cable runs up to the pitch servo and the Touch board's 8-pin FPC carries the ring LEDs and touch signals through the same joint. Turning yaw far enough will twist them. The BSP's ±128° (±410 counts) is well inside the encoder range and probably reflects exactly this.

### INA226

Power monitor at 7-bit address `0x41` on the internal I2C bus, across a 10 mΩ shunt, configured for 8.19 A full scale. Reports battery voltage and battery current, where **current is positive while discharging** and negative while charging — confirmed on hardware by watching the sign flip across a USB plug/unplug cycle. Driven from Lua via `lib_ina226`.

Note that this part answers `0x5449` (the TI manufacturer id) from the die-id register `0xFF` instead of the documented `0x2260`, so `lib_ina226` gates identity on the manufacturer id register `0xFE`. M5Unified's own driver checks `0xFF` and therefore rejects this chip, after which its `getBatteryVoltage()` / `getBatteryCurrent()` silently return `0.0f`.

### LTR-553ALS

The combined ambient light and proximity sensor is on the CoreS3 itself, at the part's fixed address `0x23` on the internal I2C bus. Driven from Lua via `lib_ltr553`.

### Si12T head touch

The Touch board carries a Si12T 12-channel capacitive touch IC at `0x68` (`ID_SEL` tied to GND), alongside the 12 ring LEDs — both reach the POWER board over the same 8-pin FPC. Driven from Lua via `lib_si12t_touch`.

**There is only one touch electrode, on the top of the head.** The chip has 12 channels but StackChan does not wire 12 pads around the head, so there is no `TS1..TS12`-to-location mapping: one press reports several channels at once, and which ones depends on where the hand lands (`TS1..TS3` in the runs so far, `TS10..TS12` in an earlier one). Treat "any channel active" as a single logical zone and debounce it — the raw output chatters against the threshold while a hand rests on the head, so an undebounced reader sees dozens of press edges per touch.

`/system/scripts/stackchan_touch.lua` does exactly that, and is also the quickest way to confirm the Touch board FPC is seated: the Si12T's I2C runs on `VCC_3V3`, so it answers even when `BUS_5V` is down and the LEDs are dark.

## Sensors skill

`stackchan_sensors` (also in the board overlay, at `/system/skills/stackchan_sensors/`) exposes the two Lua-driven sensors to the agent: `scripts/sensors.lua` with `action` of `all`, `light` or `power`.

It exists because a chip driven from Lua is **invisible to the agent's device inventory**. `board_hardware_info` is generated from `board_devices.yaml`, so the LTR-553, the INA226 and the Si12T are not in it, and that document tells the model it may not speculate about hardware. Asked to read the light sensor, the model correctly concluded there was none. A skill whose `description` carries the words a user would actually say is the only thing that closes that gap. Treat that as a rule: **driving a chip from Lua removes it from the only hardware inventory the agent trusts**, so anything added that way needs a skill, or the agent will never find it. (`board_info.yaml`'s own `description` field would be the natural place for a hint, but it does not reach the generated document — `gen_board_metadata.yaml` carries only the board name, chip and device list.)

What the skill adds over the raw drivers:

- The board's own wiring and the INA226's 10 mΩ shunt, which belong to the board rather than to a driver.
- Lux bands in plain language, so the model answers "is it dark" without inventing thresholds. The part tracks relative light well; its absolute scale is uncalibrated here.
- The charge direction in words, because **current is positive while discharging** on this board and a sign is easy to read backwards.
- No state of charge. There is no SoC curve for this battery, so the skill reports voltage and direction and explicitly forbids converting either into a percentage.
- One chip failing does not hide the other; the failing address is named.

## I2C map (internal bus, port 0, SDA GPIO12 / SCL GPIO11)

| 7-bit address | Device | Declared in `board_devices.yaml` |
|---------------|--------|--------------------------------|
| `0x21` | GC0308 camera | no |
| `0x23` | LTR-553ALS light + proximity | no (Lua) |
| `0x34` | AXP2101 PMU | yes |
| `0x36` | AW88298 speaker DAC | yes, as `0x6c` |
| `0x38` | FT5x06 touch | yes, as `0x70` |
| `0x40` | ES7210 mic ADC | yes, as `0x80` |
| `0x41` | INA226 power monitor | no (Lua) |
| `0x51` | BM8563 RTC | no |
| `0x58` | AW9523B IO expander (CoreS3) | yes, as `0xb0` |
| `0x68` | Si12T head touch (Touch board) | no (Lua) |
| `0x69` | BMI270 IMU | no |
| `0x6F` | PY32 IO expander (IOE1) | yes |

Several device types in `board_devices.yaml` take 8-bit addresses, which is why the codec, touch and AW9523B entries look shifted one bit left. Custom devices such as `ioe1` and the AXP2101 power manager take 7-bit addresses.

Scripts can attach to this bus with `i2c.new(0, 12, 11, ...)` even though the board manager already created it; the underlying helper detects the existing port and will not tear it down.

## Build

```bash
cd application/edge_agent
. $IDF_PATH/export.sh
idf.py bmgr -c ./boards -b m5stack_stackchan
idf.py build
idf.py flash monitor
```

## Bring-up scripts

Run these from the agent's Lua capability to check each subsystem. They are bundled into the `builtin_lua_modules` skill under `scripts/builtin/test/`.

| Script | Checks |
|--------|--------|
| `py32_ioe_basic.lua` | IOE1 identity, ADC channels, ring LEDs, and optionally cycles `VM_EN` |
| `scs_servo_probe.lua` | Pings both servos and reports position / load / temperature; pass `move_delta` to exercise motion |
| `ina226_read.lua` | Battery voltage, current and power |
| `ltr553_read.lua` | Ambient light in lux plus raw CH0/CH1 |

Board-specific scripts live in this board's `fatfs_image/` overlay and land at `/system/scripts/`:

| Script | Purpose |
|--------|---------|
| `stackchan_5v_check.lua` | Reads back the AW9523B and reports whether `BUS_EN` / `BOOST_EN` are up; `assert_5v` re-raises them |
| `stackchan_servo_range.lua` | Measures each axis's travel with torque off, and suggests `set_pos_limits` values |
| `stackchan_battery_indicator.lua` | Charge state on the ring (red charging / green discharging / blue idle); the way to read current sign without a console |
| `stackchan_touch.lua` | Debounced head-touch monitor: press / release / hold duration for the single top-of-head zone. `raw=true` dumps per-channel samples for FPC checks |

Start with `py32_ioe_basic.lua`: if the servos do not answer, an unpowered `VM_EN` rail is the usual cause.

## Head motion skill

The board overlay also ships a skill, `stackchan_head`, which lands at `/system/skills/stackchan_head/`. It is the agent-facing head-motion layer: one script, `scripts/head.lua`, taking an `action` (`status`, `move`, `center`, `nod`, `shake`, `torque`, `jog`, `calibrate_zero`, `calibrate_range`, `set_direction`).

It exists because a model should be asked for "look up 30 degrees", not for a register write. What it adds on top of `lib_scs_servo`:

- **Degrees instead of counts**, measured from a stored zero (1 count = 0.3125°). Positive is up (pitch) and left (yaw).
- **Per-unit calibration in `<DATA>/config/stackchan_head.json`** — zero, travel, and direction per axis. Zero points are mechanical properties of one assembled unit, so they cannot be constants; the file is written by the calibration actions and falls back to the measured defaults above when absent or damaged. Re-flashing rewrites the DATA partition, so calibration is lost unless the file is copied out first.
- **Three different bounds, kept separate.** The *encoder* range (0..1023) is what position mode can address, and is the only thing that can tell a saturated sweep from a mechanical stop. The *envelope* (pitch 560..880, yaw the encoder range) is the outer bound any configuration may name — wider than any plausible unit, so one unit's calibration is never rejected, but far from letting a corrupt file ask for the full encoder range. The *travel* is what this unit measured, and is what commands are clamped to. A configuration naming an implausible span is discarded in favour of the defaults rather than clamped.
- **Yaw's travel never comes from a sweep.** A sweep only finds the encoder ends there; the real limit is FPC twist through the joint. The default is ±90° from zero, tighter than the BSP's ±128°.
- **Moves report where the head settled**, by waiting for `MOVING` to clear rather than for the move-time budget, and flag an axis that stopped short. The tolerance comes from the servo's own dead-band registers (`CW_DEAD`/`CCW_DEAD`).

  Measured on hardware: the dead band is **1 count** on both axes, yet **yaw stops 12–14 counts (≈4°) short of every goal**, in whichever direction it was travelling, and holds there. Idle yaw load reads ~180. That is cable and FPC drag through the yaw joint beating the servo's position loop, not servo tolerance. Pitch settles within 2–6 counts. One correction pass aimed past the goal by the residual lands yaw exactly (608 commanded, 608 reached), so the skill does that by default (`trim`).

The angle layer is deliberately Lua, in the skill, rather than C: keeping it there leaves UART 1 owned by Lua. A C-side motion layer would first have to move that ownership — declare the UART in `board_peripherals.yaml`, implement the SCS protocol in C, and reach it from Lua indirectly — which would change every existing Lua driver's interface. That is worth doing only when C-side interpolation or a boot animation actually needs it.

The skill keeps `SKILL.md` to the operating essentials and pushes the calibration procedure, the meaning of each output field, and failure triage into `references/calibration.md` and `references/troubleshooting.md`, which the model reads on demand. That keeps the always-resident part small — an activated skill's `SKILL.md` rides along on every request — so the skill declares `cap_files` alongside `cap_lua`.

The angle layer was checked away from the device by driving the real `lib_scs_servo` against a two-servo simulator on a host build of the vendored Lua — that harness is not part of this repository. Every number quoted above comes from a run on hardware.

## Known limitations

- **Only the UART servo path is supported.** M5Stack also documents a newer StackChan V1.1 revision where a coprocessor at I2C `0x3A` takes over both the servos and the RGB ring, exposing them as a register file. That revision is intentionally out of scope here; nothing in this board definition talks to `0x3A`.
- The ring LEDs and IOE1's ADC/PWM blocks are reachable from Lua but have no C-side or agent-facing abstraction yet.
- `board_devices.yaml` keeps the CoreS3 SD card entry commented out: SD and LCD share the SPI bus and using both crashes.
- `idf.py bmgr` reports one IO conflict warning, `display_lcd.dc_gpio_num` against `spi_master.miso_io_num` on GPIO35. That is inherited from the CoreS3 definition and expected: the panel runs in SIO mode.
