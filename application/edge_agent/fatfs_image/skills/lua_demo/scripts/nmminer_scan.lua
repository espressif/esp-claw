--[[
nmminer_scan : LAN discovery for NMMiner / NMAxe / NMAxeGamma / NMQAxe++.

Two operating modes:

  A) Fast seeded mode  (args.seed_ips supplied)
     1. GET http://<seed>/alive on each seed (any one known NMMiner IP).
     2. Union+dedupe the returned IP lists.
     3. GET http://<ip>/probe on every unique IP.
     4. Print a compact summary table.

  B) Auto subnet mode  (args.seed_ips omitted / empty)
     1. Read this device's own STA IP (system.ip()).
     2. Derive the /24 subnet (a.b.c.0 .. a.b.c.255) from it.
     3. Sweep /alive across the subnet with a very short timeout. The first
        host that responds with a JSON IP list short-circuits the sweep.
     4. If /alive sweep yields no list, fall back to a direct /probe sweep
        of the same /24 (slower; useful when no live NMMiner is reachable
        but a single device is up).
     5. Probe + summarize as in mode A.

Args (global `args`):
  seed_ips              : table[string]  optional - explicit seeds (mode A)
  alive_timeout_ms      : int            default 2500  (mode A)
  probe_timeout_ms      : int            default 1500
  model_filter          : string         default ""    (e.g. "NMQAxe++")
  max_targets           : int            default 64    (cap on probed IPs)
  subnet_alive_timeout_ms : int          default 250   (mode B sweep)
  subnet_probe_timeout_ms : int          default 250   (mode B fallback)
  subnet_host_start     : int            default 1     (.1)
  subnet_host_end       : int            default 254   (.254)

Output: human-readable lines. The final line starts with "SUMMARY:" so the
LLM can locate it without parsing the whole table.
]]

local http = require("http")
local system = require("system")

local a = type(args) == "table" and args or {}

local explicit_seeds = a.seed_ips
local mode_auto = type(explicit_seeds) ~= "table" or #explicit_seeds == 0

local alive_to = tonumber(a.alive_timeout_ms) or 2500
local probe_to = tonumber(a.probe_timeout_ms) or 1500
local subnet_alive_to = tonumber(a.subnet_alive_timeout_ms) or 250
local subnet_probe_to = tonumber(a.subnet_probe_timeout_ms) or 250
local host_lo = tonumber(a.subnet_host_start) or 1
local host_hi = tonumber(a.subnet_host_end)   or 254
if host_lo < 1   then host_lo = 1   end
if host_hi > 254 then host_hi = 254 end

local model_filter = type(a.model_filter) == "string" and a.model_filter or ""
local max_targets = tonumber(a.max_targets) or 64
if max_targets < 1 then max_targets = 1 end

-- Pull a single string field "key":"value" out of a flat JSON line.
local function jget_string(s, key)
    if not s then return nil end
    return s:match('"' .. key .. '"%s*:%s*"([^"]*)"')
end

-- Pull a single numeric field "key":number (handles ints, floats, scientific).
local function jget_number(s, key)
    if not s then return nil end
    local v = s:match('"' .. key .. '"%s*:%s*(%-?[%d%.eE%+]+)')
    return v and tonumber(v) or nil
end

-- Extract every IPv4 dotted-quad from the response body.
-- Works for {"ips":["1.2.3.4","5.6.7.8"]} without a real JSON parser.
local function extract_ips(s)
    local out = {}
    if not s then return out end
    for ip in s:gmatch('%d+%.%d+%.%d+%.%d+') do
        out[#out + 1] = ip
    end
    return out
end

-- Format hashrate (H/s) -> short string (TH/s, GH/s, MH/s, KH/s).
local function fmt_hr(hr)
    hr = tonumber(hr) or 0
    if hr >= 1e12 then return string.format("%.2f TH/s", hr / 1e12) end
    if hr >= 1e9  then return string.format("%.2f GH/s", hr / 1e9 ) end
    if hr >= 1e6  then return string.format("%.2f MH/s", hr / 1e6 ) end
    if hr >= 1e3  then return string.format("%.2f KH/s", hr / 1e3 ) end
    return string.format("%d H/s", hr)
end

-- Dedupe set helper.
local function add_unique(set, list, ip)
    if not ip or ip == "" then return end
    if set[ip] then return end
    set[ip] = true
    list[#list + 1] = ip
end

-- Quick check that a /probe response body looks like an NMMiner.
local function probe_is_nmminer(body)
    if not body then return nil end
    local model = jget_string(body, "model")
    local ver   = jget_string(body, "ver")
    local hr    = jget_number(body, "hr")
    if model and ver and hr then return model, ver, hr end
    return nil
end

-- ======================================================================
-- Step 1+2: build the candidate IP set.
-- ======================================================================
local set, candidates = {}, {}
local seed_count_for_log = 0

if not mode_auto then
    -- Mode A: explicit seeds.
    seed_count_for_log = #explicit_seeds
    for _, seed in ipairs(explicit_seeds) do
        if type(seed) == "string" and seed ~= "" then
            add_unique(set, candidates, seed)
            local url = string.format("http://%s/alive", seed)
            local r = http.get(url, { timeout_ms = alive_to, max_body_bytes = 4096 })
            if r and r.ok and r.status == 200 and r.body then
                for _, ip in ipairs(extract_ips(r.body)) do
                    add_unique(set, candidates, ip)
                end
            else
                print(string.format("[scan] seed %s /alive failed: status=%s err=%s",
                      seed, tostring(r and r.status), tostring(r and r.error)))
            end
        end
    end
else
    -- Mode B: derive /24 from local STA IP and sweep.
    local self_ip = system.ip()
    if type(self_ip) ~= "string" or self_ip == "" or self_ip == "0.0.0.0" then
        error("nmminer_scan: no args.seed_ips supplied and local STA IP is unavailable; pass seed_ips=[...] manually")
    end
    local p1, p2, p3, p4 = self_ip:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
    if not p1 then
        error("nmminer_scan: cannot parse local IP " .. tostring(self_ip))
    end
    local prefix = string.format("%s.%s.%s.", p1, p2, p3)
    local self_host = tonumber(p4)
    print(string.format("[scan] auto mode: local IP %s -> sweeping %s%d..%s%d",
          self_ip, prefix, host_lo, prefix, host_hi))

    -- Pass 1: /alive sweep. First success gives us the full NM IP list.
    local alive_hit_ip = nil
    local sweep_seen = 0
    for h = host_lo, host_hi do
        if h ~= self_host then
            local ip = prefix .. tostring(h)
            local r = http.get("http://" .. ip .. "/alive",
                               { timeout_ms = subnet_alive_to, max_body_bytes = 4096 })
            sweep_seen = sweep_seen + 1
            if r and r.ok and r.status == 200 and r.body and r.body:find('"ips"', 1, true) then
                add_unique(set, candidates, ip)        -- the /alive responder itself
                for _, found in ipairs(extract_ips(r.body)) do
                    add_unique(set, candidates, found)
                end
                alive_hit_ip = ip
                print(string.format("[scan] /alive hit at %s after %d host(s); list size=%d",
                      ip, sweep_seen, #candidates))
                break
            end
        end
    end

    -- Pass 2: if /alive sweep failed, fall back to a direct /probe sweep.
    if not alive_hit_ip then
        print(string.format("[scan] no /alive responder found in %d host(s); falling back to /probe sweep",
              sweep_seen))
        for h = host_lo, host_hi do
            if h ~= self_host then
                local ip = prefix .. tostring(h)
                local r = http.get("http://" .. ip .. "/probe",
                                   { timeout_ms = subnet_probe_to, max_body_bytes = 2048 })
                if r and r.ok and r.status == 200 and r.body and probe_is_nmminer(r.body) then
                    add_unique(set, candidates, ip)
                end
            end
        end
        print(string.format("[scan] /probe fallback found %d candidate(s)", #candidates))
    end
    seed_count_for_log = 0
end

if #candidates > max_targets then
    print(string.format("[scan] candidate IPs %d > max_targets %d, truncating",
          #candidates, max_targets))
    for i = max_targets + 1, #candidates do candidates[i] = nil end
end

print(string.format("[scan] %s union: %d unique IP(s) from %d seed(s)",
      mode_auto and "auto" or "alive",
      #candidates, seed_count_for_log))

-- ======================================================================
-- Step 3: probe each IP, classify NMMiner-compatible.
-- ======================================================================
local devices = {}        -- list of probed-and-confirmed devices
local skipped = 0         -- non-NM responders / timeouts

for _, ip in ipairs(candidates) do
    local url = string.format("http://%s/probe", ip)
    local r = http.get(url, { timeout_ms = probe_to, max_body_bytes = 2048 })
    if r and r.ok and r.status == 200 and r.body then
        local body = r.body
        local model, ver, hr = probe_is_nmminer(body)
        if model then
            if model_filter == "" or model == model_filter then
                devices[#devices + 1] = {
                    ip       = ip,
                    model    = model,
                    hostname = jget_string(body, "hostname") or "",
                    ver      = ver,
                    hr       = hr,
                    sw       = jget_number(body, "sw"),
                    sh       = jget_number(body, "sh"),
                    sbd      = jget_number(body, "sbd"),
                    ebd      = jget_number(body, "ebd"),
                    ut       = jget_number(body, "ut"),
                }
            else
                skipped = skipped + 1
            end
        else
            skipped = skipped + 1
        end
    else
        skipped = skipped + 1
    end
end

-- Sort descending by hashrate so the user sees the biggest first.
table.sort(devices, function(a, b) return (a.hr or 0) > (b.hr or 0) end)

-- ======================================================================
-- Step 4: print compact summary.
-- ======================================================================
print("---- NMMiner devices ----")
print("idx  ip               model        ver         hashrate     uptime  hostname")
for i, d in ipairs(devices) do
    print(string.format("%-4d %-16s %-12s %-11s %-12s %-7s %s",
        i,
        d.ip,
        d.model,
        d.ver,
        fmt_hr(d.hr),
        tostring(d.ut or 0) .. "s",
        d.hostname))
end
print(string.format("SUMMARY: mode=%s found=%d skipped=%d candidates=%d seeds=%d filter=%q",
      mode_auto and "auto" or "seeded",
      #devices, skipped, #candidates, seed_count_for_log, model_filter))
