-- Web search capability demo.
-- Supported args:
--   query:      optional search query, defaults to "ESP-IDF Lua capability web_search"
--   session_id: optional call context session id
--   source_cap: optional call context source cap, defaults to "lua_web_search_demo"
local capability = require("capability")

local a = type(args) == "table" and args or {}

local function string_arg(key, default)
    local value = a[key]
    if type(value) == "string" and value ~= "" then
        return value
    end
    return default
end

local function build_opts()
    local opts = {
        source_cap = string_arg("source_cap", "lua_web_search_demo"),
    }

    local session_id = string_arg("session_id", nil)
    if session_id then
        opts.session_id = session_id
    end

    return opts
end

local function run()
    local query = string_arg("query", "ESP-IDF Lua capability web_search")
    local ok, out, err

    print(string.format("[call_web_search] query: %s", query))
    ok, out, err = capability.call("web_search", {
        query = query,
    }, build_opts())
    print(string.format(
        "[call_web_search] result ok=%s out=%s err=%s",
        tostring(ok), tostring(out), tostring(err)))

    if not ok then
        error(string.format("web_search failed: %s", tostring(err or out)))
    end

    print("[call_web_search] done")
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    error(err)
end
