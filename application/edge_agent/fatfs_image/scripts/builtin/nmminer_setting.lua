--[[
nmminer_setting : model-aware settings updater for NMMiner and AxeOS devices.

Discovery flow:
  1. If args.ip or args.ips is supplied, use those IPs directly.
  2. Otherwise call /alive first to collect the LAN "ips" list, then /probe each IP.
  3. Use /probe model + hr to classify each device before changing settings.
  4. For every endpoint, GET current settings first, then PATCH only changed fields.

Common args:
  ip | ips | seed_ips       : target IP(s); omit to scan the local /24 with /alive
  model | model_filter      : only modify this exact /probe model
  models                    : array of exact model names
    family                    : "nmminer" | "nm_axeos" | "bitaxe" | "axeos"
  hash_class | power_class  : "low" (<1 GH/s) | "high" (>=1 GH/s), based on /probe hr
  timeout_ms                : per-request timeout, default 3000

Mining pool args:
  pool, address             : default primary pool URL and address/user
  secondary=true            : make pool/address target the secondary/fallback pool
  primary_pool/address/password, secondary_pool/address/password are also accepted

High-ASIC args:
  freq | asicFreqReq        : asicFreqReq MHz (AxeOS only)
  vcore | asicVcoreReq      : asicVcoreReq mV (AxeOS only)

Preference args:
  brightness | Brightness, rotate_screen | RotateScreen,
  led_enable | LedEnable, screen_saver | ScreenSaver

Market args:
  main_coin, watch_coins, kline_rotate, price_page_mode
]]

local http = require("http")
local ok_system, system = pcall(require, "system")

local a = type(args) == "table" and args or {}
local timeout = tonumber(a.timeout_ms) or 3000
local probe_timeout = tonumber(a.probe_timeout_ms) or timeout
local alive_timeout = tonumber(a.alive_timeout_ms) or 500
local subnet_alive_timeout = tonumber(a.subnet_alive_timeout_ms) or 250
local subnet_probe_timeout = tonumber(a.subnet_probe_timeout_ms) or 250
local max_targets = tonumber(a.max_targets) or 64
if max_targets < 1 then max_targets = 1 end

local function first_non_nil(...)
    for i = 1, select("#", ...) do
        local v = select(i, ...)
        if v ~= nil then return v end
    end
    return nil
end

local function jget_string(s, key)
    if not s then return nil end
    return s:match('"' .. key .. '"%s*:%s*"([^"]*)"')
end

local function jget_number(s, key)
    if not s then return nil end
    local v = s:match('"' .. key .. '"%s*:%s*(%-?[%d%.eE%+]+)')
    return v and tonumber(v) or nil
end

local function jget_object(s, key)
    if not s then return nil end
    local _, open_pos = s:find('"' .. key .. '"%s*:%s*{')
    if not open_pos then return nil end
    local depth = 0
    for pos = open_pos, #s do
        local ch = s:sub(pos, pos)
        if ch == "{" then depth = depth + 1 end
        if ch == "}" then
            depth = depth - 1
            if depth == 0 then return s:sub(open_pos, pos) end
        end
    end
    return nil
end

local function current_stratum_value(body, pool_name, field_name)
    local stratum = jget_object(body, "stratum") or body
    local pool = jget_object(stratum, pool_name)
    return jget_string(pool, field_name)
end

local function extract_ips(s)
    local out = {}
    if not s then return out end
    for ip in s:gmatch('%d+%.%d+%.%d+%.%d+') do
        out[#out + 1] = ip
    end
    return out
end

local function add_unique(set, list, ip)
    if type(ip) ~= "string" or ip == "" then return end
    if set[ip] then return end
    set[ip] = true
    list[#list + 1] = ip
end

local function json_quote(s)
    s = tostring(s)
    s = s:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t')
    return '"' .. s .. '"'
end

local function json_value(v)
    if type(v) == "table" and v.__raw_json then return v.__raw_json end
    if type(v) == "number" then return tostring(v) end
    if type(v) == "boolean" then return v and "true" or "false" end
    return json_quote(v)
end

local function raw_json(s)
    return { __raw_json = s }
end

local function json_object(fields)
    local parts = {}
    for _, field in ipairs(fields) do
        if field[2] ~= nil then
            parts[#parts + 1] = json_quote(field[1]) .. ":" .. json_value(field[2])
        end
    end
    return "{" .. table.concat(parts, ",") .. "}"
end

local function join_watchlist(v)
    if type(v) == "table" then
        local parts = {}
        for _, item in ipairs(v) do parts[#parts + 1] = tostring(item) end
        return table.concat(parts, ",")
    end
    return v
end

local function parse_duration_seconds(v)
    if type(v) == "number" then return math.floor(v) end
    if type(v) ~= "string" then return nil end
    local lower = v:lower()
    if lower == "never" or lower == "off" or lower == "disable" or lower == "disabled" then return 0 end
    local n, unit = lower:match("^(%d+)%s*([smh]?)$")
    if not n then return nil end
    n = tonumber(n)
    if unit == "m" then return n * 60 end
    if unit == "h" then return n * 3600 end
    return n
end

local function family_for_probe_model(model)
    local lower = string.lower(model or "")
    if lower:find("nmminer", 1, true) then return "nmminer" end
    if lower:find("nmaxe", 1, true) or lower:find("nmqaxe", 1, true) then
        return "nm_axeos"
    end
    if lower:find("bitaxe", 1, true) or lower:find("axe", 1, true) then
        return "bitaxe"
    end
    return "unknown"
end

local function is_nm_axeos(d)
    return d and d.family == "nm_axeos"
end

local function is_bitaxe(d)
    return d and d.family == "bitaxe"
end

local function is_any_axeos(d)
    return is_nm_axeos(d) or is_bitaxe(d)
end

local function espminer_from_system_info(ip, body)
    if not body then return nil end
    local axe_ver = jget_string(body, "axeOSVersion")
    local version = jget_string(body, "version")
    local asic = jget_string(body, "ASICModel")
    local board = jget_string(body, "boardVersion")
    if not (axe_ver or asic or board) then return nil end
    local hash_gh = jget_number(body, "hashRate") or jget_number(body, "hashRate_1m") or 0
    return {
        ip = ip,
        model = board or (asic and ("Bitaxe-" .. asic)) or "Bitaxe",
        hostname = jget_string(body, "hostname") or "",
        ver = axe_ver or version or "?",
        hr = hash_gh * 1000000000,
        family = "bitaxe",
        hash_class = hash_gh >= 1 and "high" or "low",
    }
end

local function probe_device(ip)
    local r = http.get("http://" .. ip .. "/probe", { timeout_ms = probe_timeout, max_body_bytes = 2048 })
    if r and r.ok and r.status == 200 and r.body then
        local model = jget_string(r.body, "model")
        local ver = jget_string(r.body, "ver")
        local hr = jget_number(r.body, "hr")
        if model and ver and hr then
            return {
                ip = ip,
                model = model,
                hostname = jget_string(r.body, "hostname") or "",
                ver = ver,
                hr = hr,
                family = family_for_probe_model(model),
                hash_class = hr >= 1000000000 and "high" or "low",
            }
        end
    end

    local info_r = http.get("http://" .. ip .. "/api/system/info", { timeout_ms = probe_timeout, max_body_bytes = 4096 })
    if info_r and info_r.ok and info_r.status == 200 and info_r.body then
        local d = espminer_from_system_info(ip, info_r.body)
        if d then return d end
    end

    return nil, string.format("not NM /probe or Bitaxe /api/system/info compatible; probe_status=%s info_status=%s",
        tostring(r and r.status), tostring(info_r and info_r.status))
end

local function collect_direct_ips()
    local set, ips = {}, {}
    if type(a.ip) == "string" then add_unique(set, ips, a.ip) end
    if type(a.ips) == "table" then
        for _, ip in ipairs(a.ips) do add_unique(set, ips, ip) end
    end
    return set, ips
end

local function collect_alive_ips(set, ips)
    local seeds = a.seed_ips
    if type(seeds) == "table" and #seeds > 0 then
        for _, seed in ipairs(seeds) do
            add_unique(set, ips, seed)
            local r = http.get("http://" .. tostring(seed) .. "/alive", { timeout_ms = alive_timeout, max_body_bytes = 4096 })
            if r and r.ok and r.status == 200 and r.body then
                for _, found in ipairs(extract_ips(r.body)) do add_unique(set, ips, found) end
            end
        end
        return #ips
    end

    if not (ok_system and system and type(system.ip) == "function") then
        error("nmminer_setting: no ip/ips/seed_ips supplied and system.ip() is unavailable")
    end
    local self_ip = system.ip()
    local p1, p2, p3, p4 = tostring(self_ip):match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
    if not p1 then
        error("nmminer_setting: cannot parse local IP " .. tostring(self_ip) .. "; pass ip or seed_ips")
    end
    local prefix = string.format("%s.%s.%s.", p1, p2, p3)
    local self_host = tonumber(p4)
    local host_lo = tonumber(a.subnet_host_start) or 1
    local host_hi = tonumber(a.subnet_host_end) or 254
    if host_lo < 1 then host_lo = 1 end
    if host_hi > 254 then host_hi = 254 end

    print(string.format("[setting] scanning /alive on %s%d..%s%d", prefix, host_lo, prefix, host_hi))
    for h = host_lo, host_hi do
        if h ~= self_host then
            local ip = prefix .. tostring(h)
            local r = http.get("http://" .. ip .. "/alive", { timeout_ms = subnet_alive_timeout, max_body_bytes = 4096 })
            if r and r.ok and r.status == 200 and r.body and r.body:find('"ips"', 1, true) then
                add_unique(set, ips, ip)
                for _, found in ipairs(extract_ips(r.body)) do add_unique(set, ips, found) end
                print(string.format("[setting] /alive hit %s, ips=%d", ip, #ips))
                return #ips
            end
        end
    end
    return #ips
end

local function collect_probe_fallback(set, ips)
    if #ips > 0 or type(a.seed_ips) == "table" then return end
    if not (ok_system and system and type(system.ip) == "function") then return end
    local self_ip = system.ip()
    local p1, p2, p3, p4 = tostring(self_ip):match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
    if not p1 then return end
    local prefix = string.format("%s.%s.%s.", p1, p2, p3)
    local self_host = tonumber(p4)
    print("[setting] /alive found no ips; falling back to /probe sweep")
    for h = 1, 254 do
        if h ~= self_host then
            local ip = prefix .. tostring(h)
            local r = http.get("http://" .. ip .. "/probe", { timeout_ms = subnet_probe_timeout, max_body_bytes = 2048 })
            if r and r.ok and r.status == 200 and r.body and jget_string(r.body, "model") and jget_number(r.body, "hr") then
                add_unique(set, ips, ip)
            else
                local info_r = http.get("http://" .. ip .. "/api/system/info",
                    { timeout_ms = subnet_probe_timeout, max_body_bytes = 2048 })
                if info_r and info_r.ok and info_r.status == 200 and info_r.body and espminer_from_system_info(ip, info_r.body) then
                    add_unique(set, ips, ip)
                end
            end
        end
    end
end

local function matches_filter(d)
    local model_filter = first_non_nil(a.model, a.model_filter)
    if type(model_filter) == "string" and model_filter ~= "" and d.model ~= model_filter then return false end
    if type(a.models) == "table" and #a.models > 0 then
        local ok = false
        for _, m in ipairs(a.models) do if d.model == m then ok = true end end
        if not ok then return false end
    end
    local family_filter = a.family
    if type(family_filter) == "string" and family_filter ~= "" then
        if family_filter == "axeos" then
            if not is_any_axeos(d) then return false end
        elseif d.family ~= family_filter then
            return false
        end
    end
    local class_filter = first_non_nil(a.hash_class, a.power_class)
    if type(class_filter) == "string" and class_filter ~= "" and d.hash_class ~= class_filter then return false end
    return true
end

local function has_pool_change()
    return first_non_nil(a.pool, a.address, a.user, a.wallet, a.primary_pool, a.primary_address,
        a.primary_password, a.secondary_pool, a.secondary_address, a.secondary_password,
        a.PrimaryPool, a.PrimaryAddress, a.PrimaryPassword, a.SecondaryPool,
        a.SecondaryAddress, a.SecondaryPassword) ~= nil
end

local function add_field(fields, key, value)
    if value ~= nil then fields[#fields + 1] = { key, value } end
end

local function build_mining_body(d, errors, current_body)
    local fields = {}
    local freq = tonumber(first_non_nil(a.freq, a.asicFreqReq))
    local vcore = tonumber(first_non_nil(a.vcore, a.asicVcoreReq))
    if is_bitaxe(d) then
        if freq and freq < 1 then errors[#errors + 1] = "frequency must be positive" end
        if vcore and vcore < 1 then errors[#errors + 1] = "coreVoltage must be positive" end
    else
        if freq and (freq < 400 or freq > 600) then errors[#errors + 1] = "freq out of range 400..600" end
        if vcore and (vcore < 1100 or vcore > 1400) then errors[#errors + 1] = "vcore out of range 1100..1400" end
    end

    if freq or vcore then
        if is_nm_axeos(d) then
            if freq then add_field(fields, "asicFreqReq", math.floor(freq)) end
            if vcore then add_field(fields, "asicVcoreReq", math.floor(vcore)) end
        elseif is_bitaxe(d) then
            if freq then add_field(fields, "frequency", math.floor(freq)) end
            if vcore then add_field(fields, "coreVoltage", math.floor(vcore)) end
            add_field(fields, "overclockEnabled", 1)
        else
            errors[#errors + 1] = "ASIC freq/vcore is only supported on AxeOS/Bitaxe models"
        end
    end

    if has_pool_change() then
        local slot = tostring(first_non_nil(a.pool_slot, a.pool_role, a.pool_type, "primary")):lower()
        local default_secondary = a.secondary == true or slot == "secondary" or slot == "fallback"
        local primary_pool = first_non_nil(a.primary_pool, a.PrimaryPool)
        local primary_address = first_non_nil(a.primary_address, a.PrimaryAddress)
        local primary_password = first_non_nil(a.primary_password, a.PrimaryPassword)
        local secondary_pool = first_non_nil(a.secondary_pool, a.SecondaryPool)
        local secondary_address = first_non_nil(a.secondary_address, a.SecondaryAddress)
        local secondary_password = first_non_nil(a.secondary_password, a.SecondaryPassword)
        if default_secondary then
            secondary_pool = first_non_nil(secondary_pool, a.pool)
            secondary_address = first_non_nil(secondary_address, a.address, a.user, a.wallet)
            secondary_password = first_non_nil(secondary_password, a.password)
        else
            primary_pool = first_non_nil(primary_pool, a.pool)
            primary_address = first_non_nil(primary_address, a.address, a.user, a.wallet)
            primary_password = first_non_nil(primary_password, a.password)
        end

        if d.family == "nmminer" then
            add_field(fields, "PrimaryPool", primary_pool)
            add_field(fields, "PrimaryAddress", primary_address)
            add_field(fields, "PrimaryPassword", primary_password)
            add_field(fields, "SecondaryPool", secondary_pool)
            add_field(fields, "SecondaryAddress", secondary_address)
            add_field(fields, "SecondaryPassword", secondary_password)
        elseif is_nm_axeos(d) then
            local stratum = {}
            local wants_primary = primary_pool ~= nil or primary_address ~= nil or primary_password ~= nil
            local wants_fallback = secondary_pool ~= nil or secondary_address ~= nil or secondary_password ~= nil
            if wants_primary then
                local primary = json_object({
                    { "url", first_non_nil(primary_pool, current_stratum_value(current_body, "primary", "url")) },
                    { "user", first_non_nil(primary_address, current_stratum_value(current_body, "primary", "user")) },
                    { "pwd", first_non_nil(primary_password, current_stratum_value(current_body, "primary", "pwd")) },
                })
                stratum[#stratum + 1] = { "primary", raw_json(primary) }
            end
            if wants_fallback then
                local fallback = json_object({
                    { "url", first_non_nil(secondary_pool, current_stratum_value(current_body, "fallback", "url")) },
                    { "user", first_non_nil(secondary_address, current_stratum_value(current_body, "fallback", "user")) },
                    { "pwd", first_non_nil(secondary_password, current_stratum_value(current_body, "fallback", "pwd")) },
                })
                stratum[#stratum + 1] = { "fallback", raw_json(fallback) }
            end
            if #stratum > 0 then add_field(fields, "stratum", raw_json(json_object(stratum))) end
        elseif is_bitaxe(d) then
            add_field(fields, "stratumURL", primary_pool)
            add_field(fields, "stratumUser", primary_address)
            add_field(fields, "stratumPassword", primary_password)
            add_field(fields, "fallbackStratumURL", secondary_pool)
            add_field(fields, "fallbackStratumUser", secondary_address)
            add_field(fields, "fallbackStratumPassword", secondary_password)
        else
            errors[#errors + 1] = "unknown model family for mining settings"
        end
    end

    if #fields == 0 then return nil end
    return json_object(fields)
end

local function build_preference_body(d, errors)
    local fields = {}
    local brightness = tonumber(first_non_nil(a.brightness, a.Brightness))
    if brightness and (brightness < 1 or brightness > 100) then errors[#errors + 1] = "Brightness out of range 1..100" end
    local rotate = first_non_nil(a.rotate_screen, a.RotateScreen)
    local led = first_non_nil(a.led_enable, a.LedEnable)
    local saver = first_non_nil(a.screen_saver, a.ScreenSaver, a.screensaver, a.screensaver_timeout)

    if d.family == "nmminer" then
        add_field(fields, "Brightness", brightness and math.floor(brightness) or nil)
        add_field(fields, "RotateScreen", rotate)
        add_field(fields, "LedEnable", led)
        add_field(fields, "ScreenSaver", saver)
    elseif is_nm_axeos(d) then
        add_field(fields, "Brightness", brightness and math.floor(brightness) or nil)
        if led ~= nil then add_field(fields, "ledIndicator", led) end
        if rotate ~= nil then
            local n = tonumber(rotate)
            if n == 0 then add_field(fields, "screenFlip", 0)
            elseif n == 180 then add_field(fields, "screenFlip", 1)
            else errors[#errors + 1] = "AxeOS RotateScreen supports only 0 or 180 via screenFlip" end
        end
        if saver ~= nil then
            local seconds = parse_duration_seconds(saver)
            if seconds == nil then
                errors[#errors + 1] = "AxeOS ScreenSaver must be seconds, never/off, or a duration like 5m"
            elseif seconds <= 0 then
                add_field(fields, "screensaverEnable", 0)
            else
                add_field(fields, "screensaverEnable", 1)
                add_field(fields, "screensaverTimeout", seconds)
            end
        end
        add_field(fields, "screenFlip", first_non_nil(a.screen_flip, a.screenFlip))
        add_field(fields, "screenAutoRoll", first_non_nil(a.screen_auto_roll, a.screenAutoRoll))
        add_field(fields, "screensaverMode", first_non_nil(a.screensaver_mode, a.screensaverMode))
    elseif is_bitaxe(d) then
        if brightness ~= nil or led ~= nil then
            errors[#errors + 1] = "Bitaxe ESP-Miner OpenAPI does not expose Brightness or LedEnable"
        end
        if rotate ~= nil then add_field(fields, "rotation", tonumber(rotate) or rotate) end
        if saver ~= nil then
            local seconds = parse_duration_seconds(saver)
            if seconds == nil then
                errors[#errors + 1] = "Bitaxe ScreenSaver must be minutes/seconds, never/off, or a duration like 5m"
            elseif seconds <= 0 then
                add_field(fields, "displayTimeout", -1)
            else
                add_field(fields, "displayTimeout", math.max(1, math.floor((seconds + 59) / 60)))
            end
        end
        add_field(fields, "invertscreen", first_non_nil(a.invert_screen, a.invertscreen))
    end

    if #fields == 0 then return nil end
    return json_object(fields)
end

local function build_market_body(d)
    local fields = {}
    local main_coin = first_non_nil(a.main_coin, a.MainCoin, a.mainprice, a.mainPrice)
    local watch_coins = join_watchlist(first_non_nil(a.watch_coins, a.WatchCoins, a.coinWatchlist, a.coin_watchlist))
    local kline_rotate = first_non_nil(a.kline_rotate, a.KlineRotate)
    local price_page_mode = first_non_nil(a.price_page_mode, a.PricePageMode)

    if d.family == "nmminer" then
        add_field(fields, "MainCoin", main_coin)
        add_field(fields, "WatchCoins", watch_coins)
        add_field(fields, "KlineRotate", kline_rotate)
        add_field(fields, "PricePageMode", price_page_mode)
    elseif is_nm_axeos(d) then
        add_field(fields, "mainprice", main_coin)
        add_field(fields, "coinWatchlist", watch_coins)
    elseif is_bitaxe(d) then
        if main_coin ~= nil or watch_coins ~= nil or kline_rotate ~= nil or price_page_mode ~= nil then
            return nil, "Bitaxe ESP-Miner OpenAPI does not expose market display settings"
        end
    end

    if #fields == 0 then return nil end
    return json_object(fields)
end

local function request_json(ip, path, body, get_path)
    local url = "http://" .. ip .. path
    local get_r = http.get("http://" .. ip .. (get_path or path), { timeout_ms = timeout, max_body_bytes = 4096 })
    if not (get_r and get_r.ok and get_r.status >= 200 and get_r.status < 300) then
        return false, string.format("GET %s failed status=%s err=%s", path, tostring(get_r and get_r.status), tostring(get_r and get_r.error))
    end
    if type(body) == "function" then
        local build_err
        body, build_err = body(get_r.body or "")
        if not body then
            return false, build_err or "failed to build PATCH body from current settings"
        end
    end
    local r = http.request{
        url = url,
        method = "PATCH",
        body = body,
        timeout_ms = timeout,
        max_body_bytes = 1024,
        headers = { ["Content-Type"] = "application/json" },
    }
    local method = "PATCH"
    local status = r and r.status or 0
    if not (r and r.ok and status >= 200 and status < 300) and (status == 404 or status == 405 or status == 501) then
        r = http.request{
            url = url,
            method = "POST",
            body = body,
            timeout_ms = timeout,
            max_body_bytes = 1024,
            headers = { ["Content-Type"] = "application/json" },
        }
        method = "POST"
        status = r and r.status or 0
    end
    local ok_http = r and r.ok and status >= 200 and status < 300
    if not ok_http then
        return false, string.format("%s %s failed status=%s err=%s body=%s",
            method, path, tostring(status), tostring(r and r.error), body)
    end
    return true, string.format("%s %s status=%s", method, path, tostring(status))
end

local set, candidate_ips = collect_direct_ips()
if #candidate_ips == 0 then
    collect_alive_ips(set, candidate_ips)
    collect_probe_fallback(set, candidate_ips)
end
if #candidate_ips > max_targets then
    for i = max_targets + 1, #candidate_ips do candidate_ips[i] = nil end
end
if #candidate_ips == 0 then
    error("nmminer_setting: no target IPs found")
end

local devices, skipped = {}, 0
for _, ip in ipairs(candidate_ips) do
    local d, err = probe_device(ip)
    if d and matches_filter(d) then
        devices[#devices + 1] = d
    else
        skipped = skipped + 1
        if err then print(string.format("[setting] skip %s: %s", ip, err)) end
    end
end
if #devices == 0 then
    error("nmminer_setting: no probed devices matched the requested model/family/hash_class filters")
end

local changed, failed = 0, 0
for _, d in ipairs(devices) do
    local errors = {}
    local jobs = {}
    local mining = build_mining_body(d, errors)
    local preference = build_preference_body(d, errors)
    local market, market_err = build_market_body(d)
    if mining then
        jobs[#jobs + 1] = {
            name = "mining",
            path = is_bitaxe(d) and "/api/system" or "/api/setting/mining",
            get_path = is_bitaxe(d) and "/api/system/info" or nil,
            body = function(current_body)
                local job_errors = {}
                local body = build_mining_body(d, job_errors, current_body)
                if #job_errors > 0 then return nil, table.concat(job_errors, "; ") end
                return body
            end,
        }
    end
    if preference then
        jobs[#jobs + 1] = {
            name = "preference",
            path = is_bitaxe(d) and "/api/system" or "/api/setting/preference",
            get_path = is_bitaxe(d) and "/api/system/info" or nil,
            body = preference,
        }
    end
    if market then jobs[#jobs + 1] = { name = "market", path = "/api/setting/market", body = market } end
    if market_err then errors[#errors + 1] = market_err end

    if #errors > 0 then
        failed = failed + 1
        print(string.format("RESULT: ip=%s model=%s class=%s ok=false error=%s",
            d.ip, d.model, d.hash_class, table.concat(errors, "; ")))
    elseif #jobs == 0 then
        failed = failed + 1
        print(string.format("RESULT: ip=%s model=%s class=%s ok=false error=no supported setting fields requested",
            d.ip, d.model, d.hash_class))
    else
        local ok_device = true
        for _, job in ipairs(jobs) do
            local ok, msg = request_json(d.ip, job.path, job.body, job.get_path)
            print(string.format("RESULT: ip=%s model=%s family=%s class=%s endpoint=%s ok=%s %s",
                d.ip, d.model, d.family, d.hash_class, job.name, tostring(ok), msg))
            if not ok then ok_device = false end
        end
        if ok_device then changed = changed + 1 else failed = failed + 1 end
    end
end

print(string.format("SUMMARY: candidates=%d matched=%d changed=%d failed=%d skipped=%d",
    #candidate_ips, #devices, changed, failed, skipped))
if first_non_nil(a.freq, a.asicFreqReq) then
    print("NOTE: asicFreqReq is saved to NVS and usually requires restart to take effect.")
end
