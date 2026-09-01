-- StackChan battery indicator: show charge state and level on the 12-LED ring.
--
-- Written to settle one open question without a serial console: the INA226 sign
-- convention. The BSP comments claim "positive = discharging", but at 4.125 V
-- with USB attached the battery is essentially full and not charging, so the
-- observed positive reading does not distinguish the two cases. Unplug USB, draw
-- the battery down a little, plug back in, and watch the ring colour flip.
--
--   red   = charging     (current < 0)
--   green = discharging  (current > 0)
--   blue  = idle / full  (|current| below the threshold)
--
-- Lit LED count encodes the level across `v_min`..`v_max`.
--
--   lua --run --path /system/scripts/stackchan_battery_indicator.lua
--   lua --run --path /system/scripts/stackchan_battery_indicator.lua --args-json "{\"samples\":600,\"interval_ms\":500}"

local i2c = require("i2c")
local delay = require("delay")
local ina226 = require("lib_ina226")
local py32_ioe = require("lib_py32_ioe")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end
local function num_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return v
    end
    return default
end

local PORT = int_arg("port", 0)
local SDA = int_arg("sda", 12)
local SCL = int_arg("scl", 11)
local INA_ADDR = int_arg("ina_addr", 0x41)
local IOE1_ADDR = int_arg("ioe1_addr", 0x6F)
local SHUNT_RES = num_arg("shunt_res", 0.01)
local MAX_CURRENT = num_arg("max_expected_current", 8.19)
local LED_COUNT = int_arg("led_count", 12)
local SAMPLES = int_arg("samples", 120)
local INTERVAL_MS = int_arg("interval_ms", 500)
-- Below this the battery is treated as neither charging nor discharging. 50 mA
-- comfortably clears the ~20 mA idle trickle seen on a full pack.
local IDLE_A = num_arg("idle_threshold_a", 0.05)
local V_MIN = num_arg("v_min", 3.3)
local V_MAX = num_arg("v_max", 4.2)
local BRIGHTNESS = int_arg("brightness", 48)

local bus
local monitor
local ioe
local owns_bus = false

local function cleanup()
    if ioe then
        pcall(function()
            ioe:fill_leds(0, 0, 0)
            ioe:close()
        end)
        ioe = nil
    end
    if monitor then
        pcall(function() monitor:close() end)
        monitor = nil
    end
    if owns_bus and bus then
        pcall(function() bus:close() end)
        bus = nil
    end
end

-- Returns a label plus the RGB triple for the current charge state.
local function state_for(current)
    if current < -IDLE_A then
        return "charging", BRIGHTNESS, 0, 0
    elseif current > IDLE_A then
        return "discharging", 0, BRIGHTNESS, 0
    end
    return "idle", 0, 0, BRIGHTNESS
end

-- How many LEDs to light for a given pack voltage, always at least one so the
-- colour stays visible at empty.
local function level_leds(voltage)
    local fraction = (voltage - V_MIN) / (V_MAX - V_MIN)
    if fraction < 0.0 then fraction = 0.0 end
    if fraction > 1.0 then fraction = 1.0 end
    local lit = math.floor(fraction * LED_COUNT + 0.5)
    if lit < 1 then lit = 1 end
    return lit
end

local function show(voltage, current)
    local label, r, g, b = state_for(current)
    local lit = level_leds(voltage)
    for index = 0, LED_COUNT - 1 do
        if index < lit then
            ioe:set_led_color(index, r, g, b)
        else
            ioe:set_led_color(index, 0, 0, 0)
        end
    end
    ioe:refresh_leds()
    return label, lit
end

local function run()
    if a.bus then
        bus = a.bus
    else
        bus = i2c.new(PORT, SDA, SCL, 400000)
        owns_bus = true
    end

    monitor = ina226.new({
        bus = bus,
        addr = INA_ADDR,
        shunt_res = SHUNT_RES,
        max_expected_current = MAX_CURRENT,
    })
    ioe = py32_ioe.new({bus = bus, addr = IOE1_ADDR})
    ioe:set_led_count(LED_COUNT)

    print(string.format(
        "[battery] INA226 0x%02X shunt=%.4f ohm, ring=%d LEDs, idle threshold=%.0f mA",
        monitor:address(), SHUNT_RES, LED_COUNT, IDLE_A * 1000))
    print("[battery] red=charging  green=discharging  blue=idle/full")
    print(string.format("[battery] level maps %.2f V .. %.2f V onto the ring", V_MIN, V_MAX))

    local last_label = nil
    for i = 1, SAMPLES do
        local s = monitor:read()
        local label, lit = show(s.bus_voltage, s.current)

        -- Print every sample while a console is attached, and call out
        -- transitions so they are easy to spot in a long capture.
        print(string.format("[battery] #%d %.3f V %+.3f A %.3f W -> %s (%d/%d lit)%s",
            i, s.bus_voltage, s.current, s.power, label, lit, LED_COUNT,
            (last_label and label ~= last_label) and "  <-- STATE CHANGE" or ""))
        last_label = label

        if i < SAMPLES then
            delay.delay_ms(INTERVAL_MS)
        end
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    print("[battery] ERROR: " .. (tostring(err):gsub("\n.*", "")))
    error(err)
end
