# ESP-Claw Lua LVGL Web Simulator

This directory contains the Emscripten build for the ESP-Claw Lua LVGL
simulator. It intentionally keeps only simulator-specific source files in the
repository.

The simulator supports Lua LVGL and the basic `require("display")` screen APIs.
Display supports RGB565, single/double buffering, primitives, basic and DFN1
fonts, clipping, translation, and single-touch input. Image loading is not
supported. RAW display and LVGL use exclusive screen sessions.

Runtime state is cleaned up when a script returns, fails, or stops. Switching
Apps or resolution discards pending refreshes. Browser audio does not provide
`audio.open_output()`; Apps with optional sound can run silently.

## Hardware mocks

These APIs return simulated data and do not access physical devices:

- `imu.new({ device = "imu_sensor" })` opens a named mock board sensor.
- `imu.new({ chip = "bmi270", bus = bus })` opens a mock external sensor;
  `icm42670` and `mpu6050` are also accepted. Use a bus from `i2c.new(...)`
  or `i2c.borrow(port)`. Optional `address`, `int_gpio`, and `sdo_gpio` fields
  do not configure hardware.
- IMU handles provide `read()`, `read_temperature()`, `read_int_status()`,
  `name()`, and `close()`. Reads return stationary acceleration
  `{x=0, y=0, z=16384}`, zero gyro, browser elapsed microseconds in `sens_time`,
  and zero status/temperature. Read and name calls fail after closing.
- `button.on(handle, event, callback)` accepts a subscription, but no physical
  button events are generated. `button.dispatch()` returns zero, and
  `button.get_key_level(handle)` returns the inactive level.

Hardware mocks do not validate board wiring, detect resource conflicts, or
model sensor-specific ranges and timing. Validate hardware behavior on a board.

## Validation

Upload `tests/display_contract.lua` and check `DISPLAY_CONTRACT_OK`.
Duplicate screen-open errors are expected. Upload `tests/display_font_contract.lua`
and check `DISPLAY_FONT_CONTRACT_OK`; truncated-font errors are expected.

Third-party dependencies are fetched into the CMake build directory:

- LVGL `v9.5.0`
- Lua `5.4.8`

## Build

From an Emscripten environment:

```bash
emcmake cmake -S tools/lua_lvgl_web_sim -B build/lua_lvgl_web_sim -G Ninja -DCMAKE_BUILD_TYPE=Release
embuilder build sdl2
emmake ninja -C build/lua_lvgl_web_sim esp_claw_sim esp_claw_sim_release
```

The normal simulator page uses `esp_claw_sim.html`, `esp_claw_sim.js`,
`esp_claw_sim.wasm`, and `esp_claw_sim.data`. The release target produces a
single-file HTML build.

## Runtime Assets

The simulator-specific runtime files live in:

- `sim/` for the C/JS runtime bridge and Lua shims
- `sim/compat/` for ESP/Lua compatibility headers
- `sim_assets/` for preloaded simulator assets such as fonts
- `tools/` for local helper scripts such as the stream proxy
