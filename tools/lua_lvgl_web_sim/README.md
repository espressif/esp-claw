# ESP-Claw Lua LVGL Web Simulator

This directory contains the Emscripten build for the ESP-Claw Lua LVGL
simulator. It intentionally keeps only simulator-specific source files in the
repository.

Third-party dependencies are fetched into the CMake build directory:

- LVGL `v9.5.0`
- Lua `5.4.8`

## Build

From an Emscripten environment:

```bash
emcmake cmake -S tools/lua_lvgl_web_sim -B build/lua_lvgl_web_sim -G Ninja -DCMAKE_BUILD_TYPE=Release
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
