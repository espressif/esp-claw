local adc = require("adc")
local delay = require("delay")

local ADC_GPIO
local ch
local SAMPLE_COUNT = 5
local SAMPLE_INTERVAL_MS = 200

assert(adc.ATTEN_DB_0 == 0)
assert(adc.ATTEN_DB_12 == 3)
assert(adc.BITWIDTH_DEFAULT == 0)

for _, gpio in ipairs({ 1, 42 }) do
    local ok, channel = pcall(adc.new, gpio)
    if ok then
        ADC_GPIO = gpio
        ch = channel
        break
    end
end
assert(ch, "no test ADC GPIO available")

assert(not pcall(adc.new, ADC_GPIO, { atten = -1 }))
assert(not pcall(adc.new, ADC_GPIO, { bitwidth = 8 }))
assert(not pcall(adc.new, ADC_GPIO, { unknown = true }))
assert(not pcall(adc.new, ADC_GPIO, { [1] = adc.ATTEN_DB_0 }))
assert(not pcall(adc.new, ADC_GPIO, {}, adc.BITWIDTH_DEFAULT))

local config = ch:get_config()
assert(config.gpio == ADC_GPIO)
assert(config.atten == adc.ATTEN_DB_0)
assert(config.bitwidth > 0)
assert(type(ch:is_calibrated()) == "boolean")
assert(not pcall(adc.new, ADC_GPIO))
assert(not pcall(adc.new, ADC_GPIO, { atten = adc.ATTEN_DB_12 }))

print(string.format(
    "[adc_demo] reading gpio=%d for %d samples...",
    config.gpio,
    SAMPLE_COUNT
))

for i = 1, SAMPLE_COUNT do
    print(string.format(
        "[adc_demo] sample %d/%d: raw=%d",
        i,
        SAMPLE_COUNT,
        ch:read_raw()
    ))

    if i < SAMPLE_COUNT then
        delay.delay_ms(SAMPLE_INTERVAL_MS)
    end
end

local calibrated = ch:is_calibrated()
local mv_ok, mv_or_err = pcall(ch.read_mv, ch)
if calibrated then
    assert(mv_ok, tostring(mv_or_err))
    assert(type(mv_or_err) == "number")
else
    assert(not mv_ok)
    assert(string.find(mv_or_err, "calibration unavailable", 1, true))
end

ch:close()
ch:close()
assert(not pcall(ch.read_raw, ch))

local configured = adc.new(ADC_GPIO, {
    atten = adc.ATTEN_DB_0,
    bitwidth = adc.BITWIDTH_DEFAULT,
})
local configured_config = configured:get_config()
assert(configured_config.atten == adc.ATTEN_DB_0)
assert(configured_config.bitwidth == config.bitwidth)
configured:close()

do
    local scoped <close> = adc.new(ADC_GPIO)
    assert(scoped:get_config().gpio == ADC_GPIO)
end

local reopened = adc.new(ADC_GPIO)
reopened:close()
print("[adc_demo] done")
