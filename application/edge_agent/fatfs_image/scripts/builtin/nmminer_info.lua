--[[
nmminer_info : read live status from a single NMMiner device.

Args:
  ip         : string   required
  section    : string   optional, one of "system" (default) | "realtime" | "probe"
  timeout_ms : int      optional, default 3000

Prints a compact one-line summary plus the raw JSON (truncated to fit cap_lua
4 KB output buffer). The LLM should pass the summary line through and only
quote raw JSON fragments when the user explicitly asks for detail.
]]

local http = require("http")

local a = type(args) == "table" and args or {}
local ip = a.ip
if type(ip) ~= "string" or ip == "" then
    error("nmminer_info: missing args.ip")
end
local section = a.section or "system"
local timeout = tonumber(a.timeout_ms) or 3000

local path
if     section == "system"   then path = "/api/system/info"
elseif section == "realtime" then path = "/api/dashboard/chart/realtime"
elseif section == "probe"    then path = "/probe"
else error("nmminer_info: unknown section " .. tostring(section)) end

local url = "http://" .. ip .. path
local r = http.get(url, { timeout_ms = timeout, max_body_bytes = 3072 })
if not (r and r.ok) then
    error(string.format("nmminer_info: %s failed: status=%s err=%s",
          url, tostring(r and r.status), tostring(r and r.error)))
end

local body = r.body or ""

-- Quick scalar extraction so the LLM can answer "hashrate?" / "temp?" with
-- zero JSON parsing on its side.
local function num(key)
    return body:match('"' .. key .. '"%s*:%s*(%-?[%d%.eE%+]+)')
end
local function str(key)
    return body:match('"' .. key .. '"%s*:%s*"([^"]*)"')
end

if section == "system" then
    print(string.format("SUMMARY: ip=%s host=%s fw=%s model=%s",
        ip, str("hostName") or "?", str("fwVersion") or "?", str("hwModel") or "?"))
    print(string.format("        hashRate=%s asicTemp=%s vcoreTemp=%s power=%sW",
        num("hashRate") or "?", num("asic") or num("asic") or "?",
        num("vcore") or "?", num("power") or "?"))
    print(string.format("        accepted=%s rejected=%s blkhits=%s uptime=%ss",
        num("sAccepted") or "?", num("sRejected") or "?",
        num("blkhits") or "?", num("uptimeSeconds") or "?"))
    local pool = str("url")
    if pool then print("        pool=" .. pool) end
elseif section == "probe" then
    print(string.format("SUMMARY: ip=%s model=%s ver=%s host=%s hr=%s ut=%ss",
        ip, str("model") or "?", str("ver") or "?",
        str("hostname") or "?", num("hr") or "?", num("ut") or "?"))
end

print("RAW:" .. body)
if r.truncated then print("(body truncated to fit response cap)") end
