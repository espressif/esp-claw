-- Generic capability.call demo.
-- Supported args:
--   cap_name:     target capability id/name
--   payload:      optional Lua table payload
--   payload_json: optional JSON string payload, used when payload is not a table
--   session_id:   optional call context session id
--   channel:      optional call context channel
--   chat_id:      optional call context chat id
--   source_cap:   optional call context source cap, defaults to "lua_builtin_demo"
local capability = require("capability")

local a = type(args) == "table" and args or {}

local function string_arg(key, default)
    local value = a[key]
    if type(value) == "string" and value ~= "" then
        return value
    end
    return default
end

local function build_call_opts()
    local opts = {
        source_cap = string_arg("source_cap", "lua_builtin_demo"),
    }

    local session_id = string_arg("session_id", nil)
    local channel = string_arg("channel", nil)
    local chat_id = string_arg("chat_id", nil)

    if session_id then
        opts.session_id = session_id
    end
    if channel then
        opts.channel = channel
    end
    if chat_id then
        opts.chat_id = chat_id
    end

    return opts
end

local function build_payload()
    if type(a.payload) == "table" then
        return a.payload
    end

    local payload_json = string_arg("payload_json", nil)
    if payload_json then
        return payload_json
    end

    return nil
end

local function run()
    local cap_name = string_arg("cap_name", nil)
    local payload = build_payload()
    local ok, out, err

    if not cap_name then
        error("[basic_capability_call] args.cap_name is required")
    end

    print(string.format("[basic_capability_call] calling capability: %s", cap_name))
    ok, out, err = capability.call(cap_name, payload, build_call_opts())
    print(string.format(
        "[basic_capability_call] result cap=%s ok=%s out=%s err=%s",
        tostring(cap_name), tostring(ok), tostring(out), tostring(err)))

    if not ok then
        error(string.format("%s failed: %s", cap_name, tostring(err or out)))
    end

    print("[basic_capability_call] done")
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    error(err)
end
