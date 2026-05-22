--[[
NM-CYD-C5 GPS reader (LP-UART on the P5 connector).

Pinout per the NM-CYD-C5 documentation:
    GPS module RX  <- ESP32 TX = GPIO4
    GPS module TX  -> ESP32 RX = GPIO5
    Default baud   = 9600 (most NMEA modules, e.g. NM-ATGM336H)

This script opens UART1 on those pins, reads NMEA sentences for a few
seconds and parses the most recent fix (latitude, longitude, UTC time,
fix quality, satellites in view) out of $GxGGA / $GxRMC.

Returns a table with the latest parsed fix when called from another
script:
    local gps = dofile("/fatfs/scripts/builtin/nm_cyd_c5_gps.lua")
    print(gps.lat, gps.lon)

When invoked directly via `run_lua_script script="nm_cyd_c5_gps"` it
prints a short report and exits.
]]

local uart = require("uart")
local delay = require("delay")

local GPS_UART_PORT = 1
local GPS_TX_GPIO   = 5    -- ESP32 TX -> GPS RX
local GPS_RX_GPIO   = 4    -- ESP32 RX <- GPS TX
local GPS_BAUD      = 9600
local READ_TIMEOUT_MS = 500
local TOTAL_READ_MS   = 5000

local function split(line, sep)
    local out = {}
    local i = 1
    for token in string.gmatch(line, "([^" .. sep .. "]*)") do
        out[i] = token
        i = i + 1
    end
    return out
end

-- Convert NMEA "ddmm.mmmm" + hemisphere to signed decimal degrees.
local function nmea_to_decimal(value, hemi)
    if not value or value == "" then
        return nil
    end
    local v = tonumber(value)
    if not v then
        return nil
    end
    local deg = math.floor(v / 100)
    local minutes = v - deg * 100
    local dec = deg + minutes / 60.0
    if hemi == "S" or hemi == "W" then
        dec = -dec
    end
    return dec
end

local fix = {
    lat = nil, lon = nil,
    utc = nil, date = nil,
    quality = nil, satellites = nil, hdop = nil, altitude = nil,
    speed_knots = nil, course = nil,
    valid = false,
    sentences_seen = 0,
    sample_lines = {},
}

local function parse_gga(parts)
    -- $..GGA,utc,lat,N/S,lon,E/W,fix,sats,hdop,alt,M,...
    fix.utc        = parts[2] ~= "" and parts[2] or fix.utc
    local lat      = nmea_to_decimal(parts[3], parts[4])
    local lon      = nmea_to_decimal(parts[5], parts[6])
    fix.quality    = tonumber(parts[7]) or fix.quality
    fix.satellites = tonumber(parts[8]) or fix.satellites
    fix.hdop       = tonumber(parts[9]) or fix.hdop
    fix.altitude   = tonumber(parts[10]) or fix.altitude
    if lat then fix.lat = lat end
    if lon then fix.lon = lon end
    if fix.quality and fix.quality > 0 then
        fix.valid = true
    end
end

local function parse_rmc(parts)
    -- $..RMC,utc,status,lat,N/S,lon,E/W,speed,course,date,...
    fix.utc        = parts[2] ~= "" and parts[2] or fix.utc
    local status   = parts[3]
    local lat      = nmea_to_decimal(parts[4], parts[5])
    local lon      = nmea_to_decimal(parts[6], parts[7])
    fix.speed_knots = tonumber(parts[8]) or fix.speed_knots
    fix.course      = tonumber(parts[9]) or fix.course
    fix.date        = parts[10] ~= "" and parts[10] or fix.date
    if lat then fix.lat = lat end
    if lon then fix.lon = lon end
    if status == "A" then
        fix.valid = true
    end
end

local function dispatch(line)
    if not line or #line < 7 or line:sub(1, 1) ~= "$" then
        return
    end
    fix.sentences_seen = fix.sentences_seen + 1
    if #fix.sample_lines < 3 then
        fix.sample_lines[#fix.sample_lines + 1] = line
    end
    -- Strip trailing checksum so split() ignores it cleanly.
    local body = line:match("^(.-)%*") or line
    local parts = split(body, ",")
    local talker = parts[1]
    if talker:sub(-3) == "GGA" then
        parse_gga(parts)
    elseif talker:sub(-3) == "RMC" then
        parse_rmc(parts)
    end
end

local ok, u = pcall(uart.new, GPS_UART_PORT, GPS_TX_GPIO, GPS_RX_GPIO, GPS_BAUD)
if not ok then
    print("[nm_cyd_c5_gps] uart.new failed: " .. tostring(u))
    return fix
end
u:flush_input()

local deadline_ms = TOTAL_READ_MS
while deadline_ms > 0 do
    local line, err = u:read_line(256, READ_TIMEOUT_MS)
    if line and #line > 0 then
        local trimmed = line:gsub("[\r\n]+$", "")
        dispatch(trimmed)
    elseif err then
        print("[nm_cyd_c5_gps] read_line err: " .. tostring(err))
        break
    end
    deadline_ms = deadline_ms - READ_TIMEOUT_MS
    delay.delay_ms(1)
end
u:close()

if fix.sentences_seen == 0 then
    print("[nm_cyd_c5_gps] no NMEA sentences received; check the GPS module on P5")
else
    print(string.format("[nm_cyd_c5_gps] sentences=%d valid=%s",
                        fix.sentences_seen, tostring(fix.valid)))
    if fix.valid then
        print(string.format("[nm_cyd_c5_gps] lat=%.6f lon=%.6f sats=%s hdop=%s alt=%sm utc=%s date=%s",
                            fix.lat or 0/0, fix.lon or 0/0,
                            tostring(fix.satellites), tostring(fix.hdop),
                            tostring(fix.altitude), tostring(fix.utc),
                            tostring(fix.date)))
    else
        print("[nm_cyd_c5_gps] no fix yet (need clear sky view); sample lines:")
        for _, s in ipairs(fix.sample_lines) do
            print("    " .. s)
        end
    end
end

return fix
