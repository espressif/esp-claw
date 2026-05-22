--[[
nmminer_setting : adjust ASIC frequency / vcore on one NMMiner device.

Args:
  ip         : string  required
  freq       : int     optional (MHz, 400..600)   -> asicFreqReq (needs reboot)
  vcore      : int     optional (mV, 1100..1400)  -> asicVcoreReq (immediate)
  timeout_ms : int     optional, default 3000

At least one of freq / vcore must be provided.
]]

local http = require("http")

local a = type(args) == "table" and args or {}
local ip = a.ip
if type(ip) ~= "string" or ip == "" then
    error("nmminer_setting: missing args.ip")
end
local freq  = tonumber(a.freq)
local vcore = tonumber(a.vcore)
if not freq and not vcore then
    error("nmminer_setting: provide at least one of args.freq (MHz) or args.vcore (mV)")
end
if freq and (freq < 400 or freq > 600) then
    error("nmminer_setting: args.freq out of range 400..600")
end
if vcore and (vcore < 1100 or vcore > 1400) then
    error("nmminer_setting: args.vcore out of range 1100..1400")
end
local timeout = tonumber(a.timeout_ms) or 3000

-- Build a tiny JSON body by hand (only ints, no escaping needed).
local parts = {}
if freq  then parts[#parts + 1] = string.format('"asicFreqReq":%d',  math.floor(freq))  end
if vcore then parts[#parts + 1] = string.format('"asicVcoreReq":%d', math.floor(vcore)) end
local body = "{" .. table.concat(parts, ",") .. "}"

local url = "http://" .. ip .. "/api/setting/mining"
local r = http.request{
    url            = url,
    method         = "PATCH",
    body           = body,
    timeout_ms     = timeout,
    max_body_bytes = 1024,
    headers        = { ["Content-Type"] = "application/json" },
}

local status = r and r.status or 0
local ok_http = r and r.ok and status >= 200 and status < 300

print(string.format("SUMMARY: ip=%s freq=%s vcore=%s status=%s ok=%s",
      ip, tostring(freq), tostring(vcore), tostring(status), tostring(ok_http)))
if r and r.body and #r.body > 0 then
    print("RESP:" .. r.body)
end
if freq then
    print("NOTE: asicFreqReq requires a reboot to take effect (use nmminer_control action=restart).")
end
if not ok_http then
    error(string.format("nmminer_setting: PATCH %s failed (status=%s err=%s body=%s)",
          url, tostring(status), tostring(r and r.error), body))
end
