# arg_schema

`arg_schema` normalizes the global `args` table into predictable Lua values before a script uses hardware APIs or long-running logic.

Load it with:

```lua
local arg_schema = require("arg_schema")
```

This library is a normalization helper, not a strict validator. It does not return validation errors. Bad or missing input is converted to configured defaults.

## Typical Pattern

Define the schema once near the top of the script, parse `args` once, then use the returned context table everywhere.

```lua
local arg_schema = require("arg_schema")

local ctx = arg_schema.parse(args, {
    io = arg_schema.int({ default = 38, min = 0 }),
    enabled = arg_schema.bool({ default = true }),
    color = arg_schema.object({
        fields = {
            r = arg_schema.int({ default = 255, min = 0, max = 255 }),
            g = arg_schema.int({ default = 255, min = 0, max = 255 }),
            b = arg_schema.int({ default = 255, min = 0, max = 255 }),
        },
    }),
})
```

After parsing, read `ctx.io`, `ctx.enabled`, and `ctx.color` instead of reading `args` directly.

## `arg_schema.int(options)`

Use this for GPIOs, brightness, coordinates, counts, durations, indexes, and other numeric parameters.

Options:
- `default`: value used when the input is not a number.
- `min`: optional lower bound.
- `max`: optional upper bound.
- `floor`: numbers are floored by default. Set `floor = false` only when the target API accepts decimals.

Behavior:
- Non-number input returns `default`.
- Number input is floored unless `floor = false`.
- The result is clamped to `min` and `max` when those bounds are provided.

## `arg_schema.bool(options)`

Use this for switches and flags.

Options:
- `default`: value used when the input is not a boolean.

Behavior:
- Boolean input is preserved.
- Non-boolean input returns `default`.

## `arg_schema.object(options)`

Use this for nested groups such as colors, dimensions, or grouped hardware settings.

Options:
- `fields`: nested schema table. Each field is normalized with its own spec.
- `default`: table of fallback values copied into missing normalized fields.

Behavior:
- Table input is used as the source object.
- Non-table input is treated as an empty object.
- Only fields declared in `fields` are normalized.
- Missing normalized fields can be filled from `default`.

## `arg_schema.parse(raw_args, schema)`

Normalize `raw_args` with a schema table and return a new table.

Behavior:
- If `raw_args` is not a table, it is treated as `{}`.
- The returned table contains one normalized value for each schema key.
- The original `args` table is not mutated.

Example:

```lua
local ctx = arg_schema.parse(args, {
    duration_ms = arg_schema.int({ default = 30000, min = 1 }),
    enabled = arg_schema.bool({ default = true }),
})
```

## Practical Rules

- Always set explicit defaults for values passed to hardware APIs.
- Clamp GPIOs, colors, sizes, counters, and indexes before passing them into modules.
- Use `floor = false` only for APIs that explicitly accept decimals.
- Keep the schema close to the top of the file so script inputs are easy to inspect.
- Do not use this library when invalid input must be rejected with a user-facing error; write explicit validation for that case.
