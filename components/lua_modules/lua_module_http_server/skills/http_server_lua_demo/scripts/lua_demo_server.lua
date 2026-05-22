local http = require("http_server")
local json = require("json")

local DEFAULT_APP_ID = "lua_demo"
local DEFAULT_WEB_ROOT = "/fatfs/skills/http_server_lua_demo/assets"

local a = type(args) == "table" and args or {}
local app_id = type(a.app_id) == "string" and a.app_id ~= "" and a.app_id or DEFAULT_APP_ID
local web_root = type(a.web_root) == "string" and a.web_root ~= "" and a.web_root or DEFAULT_WEB_ROOT
local switch_enabled = false

local function safe_app_id(value)
    return type(value) == "string" and value:match("^[%w_-]+$") ~= nil
end

local function safe_abs_path(value)
    return type(value) == "string" and value:sub(1, 1) == "/" and not value:find("%.%.", 1, true)
end

local function toggle_from_body(body)
    if type(body) ~= "string" or body == "" then
        return false
    end

    local ok, data = pcall(json.decode, body)
    if not ok or type(data) ~= "table" then
        return false
    end

    return data.enabled == true
end

local function run()
    if not safe_app_id(app_id) then
        error("invalid app_id: " .. tostring(app_id))
    end
    if not safe_abs_path(web_root) then
        error("invalid web_root: " .. tostring(web_root))
    end

    local app = http.app(app_id)
    app:mount_static(web_root)

    app:get("/state", function(_req)
        return {
            json = {
                ok = true,
                enabled = switch_enabled,
            },
        }
    end)

    app:post("/toggle", function(req)
        switch_enabled = toggle_from_body(req.body)
        print("[http_server_lua_demo] " .. json.encode({
            event = "switch_toggled",
            enabled = switch_enabled,
        }))
        return {
            json = {
                ok = true,
                enabled = switch_enabled,
            },
        }
    end)

    print("[http_server_lua_demo] serving " .. app:url() .. " from " .. web_root)
    app:serve_forever()
    print("[http_server_lua_demo] stopped")
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    print("[http_server_lua_demo] ERROR: " .. tostring(err))
    error(err)
end
