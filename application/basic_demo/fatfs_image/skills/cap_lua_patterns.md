# Lua Script Patterns

Use this skill together with `cap_lua_edit` whenever you are about to write or rewrite a Lua script. These are the canonical templates for common script shapes. Start from the template that matches the user's intent, then swap in the real module calls.

All templates follow the rules in `cap_lua_edit`:
- `require` only documented modules.
- Keep emitted text simple and predictable.
- Prefer adapting an existing script before creating a brand-new one. Only emit a new script when reuse cannot satisfy the requirement.

The patterns reference representative modules only. Not every firmware build ships every module. Before emitting a script, check the active `lua_module_*` skill list and use only modules that are present. When a required module is not available, change the design rather than inventing an API.

If a built-in example under `builtin/` is close to what you need, find it through `cap_lua_list`, read it through `scripts/<relative_path>`, and usually copy the adapted version into `temp/` or `user/` instead of rewriting the built-in path in place.

## Anti-patterns (do not emit)

These examples are **wrong**. They are listed here so you can recognise and refuse them, not so you can reuse them.

```lua
-- BAD: busy-wait delay, will trip the watchdog.
for i = 1, 1000000 do end

-- BAD: blocking loop without delay, starves other tasks.
while true do
  button.dispatch()
end

-- BAD: no cleanup on error path, so any error between init and deinit
-- leaves hardware or shared state unusable for later scripts.
display.init(panel, io_h, w, h, panel_if)
display.draw_text(0, 0, "hi", { r = 255, g = 255, b = 255 })
display.present()
display.deinit()

-- BAD: float passed to a hardware API that expects integers.
display.draw_rect(w / 3, h / 3, 10, 10, 255, 0, 0)

-- BAD: using an API that does not exist. The firmware only ships
-- the modules listed in the activated lua_module_* skills.
local sock = require("socket")
```
