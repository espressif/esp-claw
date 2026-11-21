# Lua Module Component Spec

This document defines the source layout and authoring rules for components under `components/lua_modules/`.

Two naming conventions exist:

- `lua_driver_xx` — low-level hardware peripheral drivers (ADC, GPIO, I2C, LEDC, MCPWM, RMT, Touch, UART).
- `lua_module_xx` — higher-level modules (display, storage, system, etc.).

## Directory Layout

A standard component directory uses this layout:

```text
lua_driver_xx/
├── CMakeLists.txt
├── README.md
├── skills/
│   └── skill_id/
│       ├── SKILL.md
│       └── scripts/
│           └── foo.lua
├── test/
│   └── xxx.lua
├── lib/
│   ├── lib_xx.md
│   └── lib_xx.lua
└── src/
    ├── lua_driver_xx.c
    └── lua_driver_xx.h
```

```text
lua_module_xx/
├── CMakeLists.txt
├── README.md
├── skills/
│   └── skill_id/
│       ├── SKILL.md
│       └── scripts/
│           └── foo.lua
├── test/
│   └── xxx.lua
├── lib/
│   ├── lib_xx.md
│   └── lib_xx.lua
├── fs_image/
│   └── xx/
│       └── data.json
└── src/
    ├── lua_module_xx.c
    └── lua_module_xx.h
```

Required entries:

- `CMakeLists.txt`
- `README.md`

Optional entries:

- `skills/`
- `test/`
- `lib/`
- `fs_image/`
- `src/`

Do not add new top-level source directories without updating this spec and the build tooling.

## README.md Rules

`README.md` is the required module document. It must describe the module for users and AI to develop with.

It must describe:

- What the module does.
- Which Lua APIs or reusable Lua libraries it provides.
- How to call those APIs.
- Arguments, return values, error behavior, and cleanup requirements.
- Hardware resource ownership, blocking behavior, and concurrency limits when relevant.

Rules:

- Document only APIs and libraries that actually exist.
- Do not describe planned or unimplemented functions.
- Do not describe unnecessary internal implementation details.
- Keep examples short and directly runnable when possible.
- For hardware modules, include close/deinit requirements and safe GPIO guidance.

## test Directory

`test/` is optional. It stores model-readable reference scripts for tests, demos, and implementation patterns.

Rules:

- Test scripts may be used for module validation, hardware validation, demos, and examples.
- Test scripts should be self-contained when practical.
- Hardware scripts must include cleanup logic and release opened resources.

## lib Directory

`lib/` is optional. It stores reusable Lua libraries.

Rules:

- Files under `lib/abc.lua` are implementation libraries, not model-facing reading material.
- Public library functions must be documented in `lib/abc.md`.

## fs_image Directory

`fs_image/` is optional. It stores read-only data files the module loads at runtime, such as `lua_module_ir`'s IR codesets.

Rules:

- The tree is overlaid onto the root of the read-only SYSTEM image (`/system`). Put files under a module-owned subdirectory, e.g. `fs_image/ir/protocols.jsonl` -> `/system/ir/protocols.jsonl`. The image uses 4 KB clusters, so bundle many small records into one file.
- Code locates the files with `claw_paths_join(CLAW_PATH_SYSTEM, ...)`. User-writable counterparts live under the same relative path in DATA (`claw_paths_join(CLAW_PATH_DATA, ...)`), and the module decides precedence.
- Do not put Lua scripts, docs, or skills here; use `lib/`, `test/`, `README.md`, and `skills/`.

## src Directory

`src/` is optional. Use it for C implementation files and C headers related to this Lua module.

Rules:

- C bindings must register only the Lua APIs documented in `README.md`.
- C code must avoid dangling pointers and must clearly define resource ownership.
- Critical error paths should have concise log entries.

## CMakeLists.txt Rules

`CMakeLists.txt` must define the module component.

Rules:

- Pure Lua modules may use an empty `idf_component_register()`.
- C-backed modules must list their source files and include directories.

## Sync Output Rules

The build system may collect managed Lua module resources into the application filesystem image.

Expected output mapping:

- `README.md` -> `<component_name>.md` in the Lua module documentation output directory.
- `test/foo.lua` -> `test/foo.lua` in the builtin Lua output directory.
- `lib/foo.lua` -> `lib/foo.lua` in the builtin Lua output directory.
- `lib/foo.md` -> `lib/foo.md` in the builtin Lua output directory.
- `fs_image/foo/bar` -> `foo/bar` in the SYSTEM image staging root, when the application passes `SYSTEM_OUTPUT_DIR` to `lua_module_builder_configure_builtin_lua_sync()`.

Rules:

- `README.md` sync applies to components named `lua_module_*` or `lua_driver_*`.
- `lib/` and `test/` sync preserve each file's relative path inside that category.
- `fs_image/` sync skips hidden entries. A path provided by two components, or one that collides with an application or board SYSTEM file, fails the build.
- Every synced `lib/*.lua` file must have a same-name markdown doc at `lib/*.md`.
- Root-level `.lua` files are not managed sync outputs.

The exact output roots are application-specific.

## Naming And Conflict Rules

- Lua module directories must be named `lua_module_xx` or `lua_driver_xx`.
- Synced script and library output paths must be globally unique.
- A module must not maintain flat root-level `.lua` files.
