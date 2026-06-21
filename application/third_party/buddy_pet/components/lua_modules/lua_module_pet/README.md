# Pet Lua Module

The `pet` Lua module controls downloadable pet packages through the shared pet
registry and the Pet Buddy runtime. Scene modules can attach the current pet to
their own UI while `pet.action()` remains the single public action entry point.

Pet packages are discovered from mmap-assets `.bin` files under
`/fatfs/skills/pet_module/`. Each bin can contain embedded `pet.json` metadata
or use a sibling `[id].pet.json` sidecar. Metadata needs an `id`, optional
`name`, `title`, and `profile`, plus `index.json` and action assets.
For existing fixed pet bins, legacy `country` metadata is normalized into
`profile`; `pet.list()` also exposes `country` as an alias for older scripts.

Example:

```text
/fatfs/skills/pet_module/worldcup_pet_ditto.bin
```

## API

- Import it with `local pet = require("pet")`
- Call `pet.list()` to refresh and return installed pet packages
- Call `pet.select(id)` to select a pet by embedded metadata id
- Call `pet.clear()` to fall back to the built-in pet
- Call `pet.current()` to read the selected pet id, or `nil` when no pet is selected
- Call `pet.refresh()` to rescan installed pet packages and notify host runtimes
- Call `pet.active()` to check whether any module has attached the current pet
- Call `pet.action("jumping")` to change the active module pet action; if no
  module is active, the empty pet module is opened first

## Example

```lua
local pet = require("pet")

for _, item in ipairs(pet.list()) do
    print(item.id, item.name, item.asset, item.profile)
end

pet.select("worldcup_pet_ditto")
```
