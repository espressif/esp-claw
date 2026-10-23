-- Exercise a PY32 I2C IO expander: identity, ADC channels, and the LED block.
-- Defaults target M5Stack StackChan "IOE1": internal I2C bus, address 0x6F,
-- pin 0 = VM_EN (servo rail), and the 12 ring LEDs on IO14's Neopixel output.
--
-- The servo rail is left ON at the end regardless of what the test did to it.

local py32_ioe = require("lib_py32_ioe")
local delay = require("delay")
local i2c = require("i2c")

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
local FREQ_HZ = int_arg("freq_hz", 100000)
local I2C_ADDR = int_arg("addr", 0x6F)
local VM_EN_PIN = int_arg("vm_en_pin", 0)
local LED_COUNT = int_arg("led_count", 12)
local TOGGLE_SERVO_RAIL = a.toggle_servo_rail == true
local RUN_LEDS = a.leds ~= false
local PIN_AS_OUTPUT = a.pin_as_output == true

local ioe
local bus
local owns_bus = false

local function cleanup()
    if ioe then
        -- Never leave the servos unpowered because a test bailed out.
        pcall(function() ioe:write(VM_EN_PIN, true) end)
        pcall(function() ioe:close() end)
        ioe = nil
    end
    if owns_bus and bus then
        pcall(function() bus:close() end)
        bus = nil
    end
end

local function run()
    if a.bus then
        bus = a.bus
    else
        bus = i2c.new(PORT, SDA, SCL, FREQ_HZ)
        owns_bus = true
    end

    ioe = py32_ioe.new({bus = bus, addr = I2C_ADDR})
    print(string.format(
        "[py32_ioe] opened addr=0x%02X version=0x%02X uid=0x%04X pins=%d",
        ioe:address(), ioe:version(), ioe:uid(), py32_ioe.pin_count()))

    -- ADC sweep: all four channels, raw counts.
    for channel = 1, 4 do
        local ok, value = pcall(ioe.analog_read, ioe, channel)
        if ok then
            print(string.format("[py32_ioe] adc%d = %d", channel, value))
        else
            print(string.format("[py32_ioe] adc%d failed: %s", channel, tostring(value)))
        end
    end

    ioe:set_dir(VM_EN_PIN, true)
    ioe:set_pull(VM_EN_PIN, "up")
    print(string.format("[py32_ioe] VM_EN (pin %d) currently %s",
        VM_EN_PIN, tostring(ioe:read_output(VM_EN_PIN))))

    if TOGGLE_SERVO_RAIL then
        print("[py32_ioe] cutting the servo rail for 300 ms")
        ioe:write(VM_EN_PIN, false)
        delay.delay_ms(300)
        ioe:write(VM_EN_PIN, true)
        delay.delay_ms(200)
        print("[py32_ioe] servo rail restored")
    end

    if RUN_LEDS then
        -- Two references disagree about IO14. The chip datasheet and the vendor
        -- M5IOE1 library say the GPIO function and the Neopixel output are
        -- mutually exclusive, so set_led_count releases the pin. The older
        -- PY32IOExpander BSP instead configures it as a push-pull output first.
        -- `pin_as_output` selects the BSP order so hardware can settle it.
        ioe:set_led_count(LED_COUNT)
        if PIN_AS_OUTPUT then
            print("[py32_ioe] forcing IO14 to GPIO output (BSP order)")
            ioe:force_neopixel_pin_output()
        end
        delay.delay_ms(200)
        ioe:fill_leds(0, 0, 0)
        delay.delay_ms(50)
        ioe:fill_leds(0, 0, 0)
        print(string.format("[py32_ioe] led block armed, count=%d led_cfg=0x%02X",
            ioe:led_count(), ioe:led_config_raw()))

        local colours = {
            {"red", 64, 0, 0},
            {"green", 0, 64, 0},
            {"blue", 0, 0, 64},
            {"off", 0, 0, 0},
        }
        for _, c in ipairs(colours) do
            print(string.format("[py32_ioe] %d LEDs -> %s", LED_COUNT, c[1]))
            ioe:fill_leds(c[2], c[3], c[4])
            delay.delay_ms(400)
        end

        -- Walk one LED along the strip to confirm per-index addressing.
        for index = 0, LED_COUNT - 1 do
            ioe:fill_leds(0, 0, 0)
            ioe:set_led_color(index, 0, 0, 64)
            ioe:refresh_leds()
            delay.delay_ms(80)
        end
        ioe:fill_leds(0, 0, 0)
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
