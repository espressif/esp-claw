-- StackChan / CoreS3 BUS_5V bring-up check.
--
-- The 12 WS2812C ring LEDs live on the StackChan Touch board and take their
-- VDD from BUS_5V, which on CoreS3 is gated by the AW9523B expander:
--   P0_1  (logical pin 1)  = BUS_EN
--   P1_7  (logical pin 15) = BOOST_EN
-- Both must be high for the 5 V boost rail to reach the FPC. The firmware
-- raises them during board init (power_manager.c, FEATURE_5V); this script
-- reads back the expander to confirm it actually happened, and can re-assert
-- them so you can tell "rail was never on" apart from "rail is on but the
-- 3.3 V data line cannot drive a 5 V-powered WS2812C".
--
-- Board-specific by design: the pin assignments below are CoreS3 facts.
--
--   lua --run --path /system/scripts/stackchan_5v_check.lua
--   lua --run --path /system/scripts/stackchan_5v_check.lua --args-json "{\"assert_5v\":true,\"leds\":true}"

local i2c = require("i2c")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local PORT = int_arg("port", 0)
local SDA = int_arg("sda", 12)
local SCL = int_arg("scl", 11)
local AW9523B_ADDR = int_arg("aw9523b_addr", 0x58)
local IOE1_ADDR = int_arg("ioe1_addr", 0x6F)
local LED_COUNT = int_arg("led_count", 12)
local ASSERT_5V = a.assert_5v == true
local RUN_LEDS = a.leds == true

-- AW9523B register map
local REG_OUTPUT0  = 0x02
local REG_CONFIG0  = 0x04
local REG_ID       = 0x10
local REG_GCR      = 0x11
local REG_LEDMODE0 = 0x12

local PIN_BUS_EN = 1        -- P0_1
local PIN_BOOST_EN = 15     -- P1_7

local bus
local aw
local owns_bus = false

local function cleanup()
    if aw then
        pcall(function() aw:close() end)
        aw = nil
    end
    if owns_bus and bus then
        pcall(function() bus:close() end)
        bus = nil
    end
end

-- Logical pin n lives in port 0 for n < 8, port 1 otherwise.
local function pin_reg(base, pin)
    if pin < 8 then
        return base, 1 << pin
    end
    return base + 1, 1 << (pin - 8)
end

local function pin_state(pin)
    local cfg_reg, mask = pin_reg(REG_CONFIG0, pin)
    local out_reg = select(1, pin_reg(REG_OUTPUT0, pin))
    local is_input = (aw:read_byte(cfg_reg) & mask) ~= 0
    local level = (aw:read_byte(out_reg) & mask) ~= 0
    return is_input, level
end

-- Mirror of M5Unified's Power_Class::_core_s3_output: read output port 0 and 1
-- as one burst, OR in BUS_EN (P0 bit1) and BOOST_EN (P1 bit7), write both back
-- in one burst. Using the same access pattern as the known-good path rules out
-- any difference in how the two bits are sequenced.
local function assert_bus_5v()
    local raw = aw:read(2, REG_OUTPUT0)
    local p0 = string.byte(raw, 1) | (1 << PIN_BUS_EN)
    local p1 = string.byte(raw, 2) | (1 << (PIN_BOOST_EN - 8))
    aw:write(string.char(p0, p1), REG_OUTPUT0)
end

local function run()
    if a.bus then
        bus = a.bus
    else
        bus = i2c.new(PORT, SDA, SCL, 400000)
        owns_bus = true
    end
    aw = bus:device(AW9523B_ADDR, 0)

    local id = aw:read_byte(REG_ID)
    local gcr = aw:read_byte(REG_GCR)
    print(string.format("[stackchan_5v] AW9523B at 0x%02X id=0x%02X (expect 0x23) gcr=0x%02X (bit4=P0 push-pull)",
        AW9523B_ADDR, id, gcr))
    print(string.format("[stackchan_5v] config0=0x%02X config1=0x%02X (1=input)",
        aw:read_byte(REG_CONFIG0), aw:read_byte(REG_CONFIG0 + 1)))
    print(string.format("[stackchan_5v] ledmode0=0x%02X ledmode1=0x%02X (0xFF=all GPIO mode)",
        aw:read_byte(REG_LEDMODE0), aw:read_byte(REG_LEDMODE0 + 1)))
    print(string.format("[stackchan_5v] output0=0x%02X output1=0x%02X",
        aw:read_byte(REG_OUTPUT0), aw:read_byte(REG_OUTPUT0 + 1)))

    local rails = {
        {PIN_BUS_EN, "BUS_EN  ", "P0_1"},
        {PIN_BOOST_EN, "BOOST_EN", "P1_7"},
    }
    local all_high = true
    for _, rail in ipairs(rails) do
        local is_input, level = pin_state(rail[1])
        if is_input or not level then
            all_high = false
        end
        print(string.format("[stackchan_5v] %s (%s, pin %d) dir=%s level=%d",
            rail[2], rail[3], rail[1], is_input and "IN" or "out", level and 1 or 0))
    end

    if all_high then
        print("[stackchan_5v] verdict: both enables are outputs and HIGH -> BUS_5V should be up.")
        print("[stackchan_5v]   If the ring is still dark, the rail is not the problem. Next suspect")
        print("[stackchan_5v]   is the data line: IOE1 drives RGB at 3.3 V while WS2812C runs off 5 V")
        print("[stackchan_5v]   and wants V_IH around 0.7*VDD = 3.5 V. Measure BUS_5V on the FPC to confirm.")
    else
        print("[stackchan_5v] verdict: at least one enable is NOT a high output -> BUS_5V is DOWN.")
        print("[stackchan_5v]   This alone explains dark LEDs. Re-run with assert_5v to raise it.")
    end

    if ASSERT_5V then
        print("[stackchan_5v] asserting BUS_EN + BOOST_EN (single burst, as M5Unified does)")
        assert_bus_5v()
        delay.delay_ms(200)
        print(string.format("[stackchan_5v] output0=0x%02X output1=0x%02X after write",
            aw:read_byte(REG_OUTPUT0), aw:read_byte(REG_OUTPUT0 + 1)))
        for _, rail in ipairs(rails) do
            local _, level = pin_state(rail[1])
            print(string.format("[stackchan_5v] %s now level=%d", rail[2], level and 1 or 0))
        end
    end

    if RUN_LEDS then
        -- Deliberately required opt-in: only useful right after assert_5v.
        -- Note there is no pin configuration here. IO14's GPIO function and its
        -- Neopixel output are mutually exclusive, so claiming the pin as a GPIO
        -- would starve the LED engine while every register write still succeeds.
        local py32_ioe = require("lib_py32_ioe")
        local ioe = py32_ioe.new({bus = bus, addr = IOE1_ADDR})
        ioe:set_led_count(LED_COUNT)
        delay.delay_ms(200)
        ioe:fill_leds(0, 0, 0)
        delay.delay_ms(50)

        for _, c in ipairs({{"red", 64, 0, 0}, {"green", 0, 64, 0}, {"blue", 0, 0, 64}}) do
            print(string.format("[stackchan_5v] %d LEDs -> %s", LED_COUNT, c[1]))
            ioe:fill_leds(c[2], c[3], c[4])
            delay.delay_ms(600)
        end
        ioe:fill_leds(0, 0, 0)
        ioe:close()
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
