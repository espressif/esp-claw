# App Package Spec

This document defines standalone Lua App packages shown by the System UI Launcher.

## Package Layout

```text
apps/
└── app_id/
    ├── launcher.json
    ├── scripts/
    │   └── main.lua
    └── assets/
        └── icon.jpg
```

- `launcher.json` and its referenced entry script are required.
- An App owns every file in its package directory.
- Apps must not reference files owned by Skills or other Apps.
- Firmware-provided Apps live in the read-only SYSTEM App root.
- Runtime Apps live in the writable DATA App root.
- A DATA App shadows a SYSTEM App with the same id; removing the DATA override reveals the SYSTEM App again.
- App and Skill ids use independent namespaces and lifecycles.

## `launcher.json`

```json
{
  "schema_version": 1,
  "id": "light_switch",
  "display_name": "Light",
  "entry": "scripts/main.lua",
  "icon": "assets/icon.jpg",
  "args": {},
  "order": 10,
  "visible": true
}
```

- `schema_version` must be `1`.
- The encoded manifest must not exceed 4096 bytes.
- `id` is required, must contain 1–63 ASCII letters, digits, underscores, or hyphens, and must match the package directory name.
- `entry` is required and must reference an existing package-relative `.lua` file.
- `icon` is optional and accepts a package-relative `.jpg` or `.jpeg` file. A complete package should include the referenced file; the Launcher falls back to its default icon if the field or file is absent.
- `display_name` defaults to `id`.
- `args` must be an object when present.
- `order` must be an integer when present and defaults to `0`.
- `visible` must be a boolean when present and defaults to `true`.
- Package-relative paths must not be absolute, contain `..`, or contain backslashes.
- Unknown fields are ignored for forward compatibility. Preserve fields that are not being intentionally changed.
- Simulator-specific fields and inferred peripherals do not belong in `launcher.json`.
- An invalid package is logged and skipped without blocking other Apps.

## Runtime Publication

- Write all supporting files before `launcher.json` so an incomplete package is not mistaken for a finished App.
- Publish only a complete App that already exists below the DATA App root.
- Publishing validates the package and reloads the registry; it does not add missing firmware capabilities or Lua modules.
- Runtime removal only removes DATA Apps. SYSTEM Apps are read-only.
