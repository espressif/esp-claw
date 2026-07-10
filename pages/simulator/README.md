# ESP-Claw Simulator Pages

This static site is the public entry point for Lua/LVGL skills that can run in the browser simulator.

Supported URL shape:

```text
/?repo=skills-lab&ref=main&skill=skills/flappybird/SKILL.md
```

The page downloads `SKILL.md`, requires `metadata.category` to contain `ui`, reads `simulator.entry` and `simulator.files`, downloads those files, and sends a `mountSkill` / `runSkill` message to the embedded WASM runtime.

`VITE_SKILLS_LAB_WEB_BASE` must be the Skills Lab site root, not a detail route:

```text
VITE_SKILLS_LAB_WEB_BASE=https://skills-lab.esp-claw.com
```

The CI build copies generated simulator runtime files into `public/runtime/` before building:

```text
esp_claw_sim.html
esp_claw_sim.js
esp_claw_sim.wasm
esp_claw_sim.data
```
