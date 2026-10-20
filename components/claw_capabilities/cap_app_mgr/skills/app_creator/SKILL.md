---
{
  "name": "app_creator",
  "description": "Create, update, publish, or remove an on-device launcher App when the user explicitly requests a launcher-visible Lua application; not for Skills or generic Lua scripts.",
  "metadata": {
    "cap_groups": [
      "cap_files",
      "cap_lua",
      "cap_app_manage"
    ]
  }
}
---

# App Creator

Use this skill only when the user explicitly requests creation, update, or removal of a launcher-visible runtime App. Apps are independent packages consumed by the System UI Launcher; they are not Skills. Use `skill_creator` when the requested behavior should be invoked conversationally by the model instead of launched from the UI.

This workflow manages runtime Apps under the writable DATA root. It does not edit firmware-baked SYSTEM Apps or create native capabilities.

## Resolve The Runtime Apps Root

Before reading or writing a runtime App, run:

```json
{"path":"{CUR_SKILL_DIR}/scripts/resolve_apps_root.lua","args":{}}
```

Treat the returned absolute path as `<APPS_ROOT>`. Do not assume `/fatfs`; DATA may be mounted on an SD card. If the path cannot be resolved, report the error and stop.

## App Contract

Before creating or updating an App, read `{CUR_SKILL_DIR}/references/app-package-spec.md` and follow the complete package contract. Do not load the reference for removal-only requests.

Store each runtime App under `<APPS_ROOT>/<app_id>/`. Do not overwrite an existing App unless the user explicitly requested an update to that id.

Minimal manifest:

```json
{
  "schema_version": 1,
  "id": "example_app",
  "display_name": "Example App",
  "entry": "scripts/main.lua",
  "visible": true
}
```

## Create Or Update

1. Resolve `<APPS_ROOT>` and inspect the exact target package before changing it.
2. Read the package spec, then activate `lua_package_authoring` and follow its App-script rules.
3. Write scripts and existing accessible assets first, then write `launcher.json` last. Do not fabricate binary JPEG data through a text file tool; omit the icon when no valid asset is available.
4. Read the completed text files back and verify the manifest types, id, relative paths, entry existence, Lua imports, and packaged asset references.
5. Call `publish_app` with only the final id:

```json
{"app_id":"example_app"}
```

6. Treat the tool result as authoritative. Report publication errors directly and do not claim success when publication fails.

The Launcher starts the App entry asynchronously with manifest `args`, a stable job name derived from the App id, and exclusive display ownership. Do not run a long-lived UI App merely to validate its package.

## Remove

Remove an App only when the user explicitly requests deletion of that exact runtime App. Inspect the target first, then call `remove_app` with only its `app_id`. If the tool reports `readonly_app`, explain that firmware-baked SYSTEM Apps cannot be removed at runtime.
