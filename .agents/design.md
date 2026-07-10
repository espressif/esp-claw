# Design Constraints

These rules govern architectural decisions in ESP-Claw. When adding a feature or fixing a bug, prefer paths that respect these boundaries.

## Keep the agent loop small

`claw_core` is the critical runtime path: it builds iteration context, calls the LLM backend, executes capabilities, persists context, handles interrupts, and emits responses. Changes under `components/claw_modules/claw_core/` should be narrow and justified.

If a behavior can live in a capability group, Lua module, skill, router rule, board overlay, or context provider, keep it out of the core loop.

## Extend through capabilities and skills

Capabilities live under `components/claw_capabilities/` and are registered through `components/common/app_claw/app_capabilities.c`. Each capability group should keep its setup, credentials, storage paths, and registration local to that group.

Skills are user-facing instructions and assets. Built-in skill sources live under component `skills/` directories and are synced into the read-only SYSTEM image at build time. Prefer skills for model know-how and workflows; prefer capabilities for callable firmware functions.

## Keep Lua modules modular

Lua drivers and modules live under `components/lua_modules/` and are registered through `components/common/app_claw/app_lua_modules.c`. Hardware-specific modules should stay guarded by the existing Kconfig and board capability checks.

Do not add board-specific assumptions to generic Lua modules. Put board-specific setup in the board directory or in the board manager data.

## Respect filesystem layering

Shared build-time FATFS defaults live in `application/edge_agent/fatfs_image/` with one subdirectory per partition. The SYSTEM source tree (`fatfs_image/system/`) is staged into `build/system_fs_image/`, then the selected board's `fatfs_image/` overlay is copied into that SYSTEM staging directory if present. Component skills and built-in Lua scripts/docs are then synced into the SYSTEM staging directory. Board overlay content does not target writable DATA storage, and hidden board folders are not considered.

The writable DATA root is selected at boot: `/fatfs` for flash storage, or the board-manager SD card mount point when an SD card is available. Runtime code must resolve writable paths through `claw_paths` in C or the Lua `storage` module instead of hard-coding `/fatfs`.

Edit source FATFS content or board overlays, not generated staged output.
