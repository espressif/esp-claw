# Lua JSON Module

`lua_module_json` provides JSON encode and decode helpers for Lua scripts.

```lua
local json = require("json")

local text = json.encode({
    ok = true,
    value = 3,
    list = {1, 2, "x"},
})

local data = json.decode(text)
print(data.ok, data.list[3])
```

## API

### `json.encode(value) -> string`

Serializes a Lua value to a compact JSON string.

Supported values:

- `nil` -> `null`
- boolean -> JSON boolean
- number -> JSON number
- string -> JSON string
- table -> JSON array or object

Tables whose keys are sequential positive integers starting at `1` encode as JSON arrays. Other tables encode as JSON objects. Object keys may be strings or integers; integer keys are converted to string keys.

Unsupported Lua values, unsupported table keys, values nested too deeply, and allocation failures raise a Lua error.

### `json.decode(text) -> value`

Parses a JSON string and returns the corresponding Lua value.

Mappings:

- JSON object -> Lua table
- JSON array -> 1-based Lua array table
- JSON string -> Lua string
- JSON number -> Lua number
- JSON boolean -> Lua boolean
- JSON `null` -> Lua `nil`

Invalid JSON raises a Lua error.
