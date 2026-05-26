--[[
nmminer_control : control actions for one NM miner or Bitaxe ESP-Miner device.

Args:
  ip         : string  required
  action     : string  required, one of:
                  "restart"   POST /api/system/restart
                  "clearhits" POST /api/system/clearhits
                  "find"      POST /api/swarm/find, fallback POST /api/system/identify
                  "wakeup"    GET  /api/wakeup
                  "rescan"    POST /api/swarm/scan
  timeout_ms : int     optional, default 3000
]]

local http = require("http")

local a = type(args) == "table" and args or {}
local ip = a.ip
if type(ip) ~= "string" or ip == "" then
    error("nmminer_control: missing args.ip")
end
local action = a.action
if type(action) ~= "string" or action == "" then
    error("nmminer_control: missing args.action")
end
local timeout = tonumber(a.timeout_ms) or 3000

local plan = {
    restart   = { method = "POST", path = "/api/system/restart"   },
    clearhits = { method = "POST", path = "/api/system/clearhits" },
    find      = { method = "POST", path = "/api/swarm/find", fallback_path = "/api/system/identify" },
    wakeup    = { method = "GET",  path = "/api/wakeup"           },
    rescan    = { method = "POST", path = "/api/swarm/scan"       },
}
local p = plan[action]
if not p then
    error("nmminer_control: unknown action " .. tostring(action))
end

local function call_path(path)
    local url = "http://" .. ip .. path
    local r
    if p.method == "POST" then
        r = http.post(url, "{}", { timeout_ms = timeout, max_body_bytes = 1024 })
    else
        r = http.get(url, { timeout_ms = timeout, max_body_bytes = 1024 })
    end
    return r, url
end

local r, url = call_path(p.path)
local status = r and r.status or 0
if p.fallback_path and not (r and r.ok and status >= 200 and status < 300) and
    (status == 404 or status == 405 or status == 501) then
    r, url = call_path(p.fallback_path)
end

status = r and r.status or 0
local ok_http = r and r.ok and status >= 200 and status < 300

print(string.format("SUMMARY: ip=%s action=%s status=%s ok=%s",
      ip, action, tostring(status), tostring(ok_http)))
if r and r.body and #r.body > 0 then
    print("RESP:" .. r.body)
end
if not ok_http then
    error(string.format("nmminer_control: %s %s failed (status=%s err=%s)",
          p.method, url, tostring(status), tostring(r and r.error)))
end
