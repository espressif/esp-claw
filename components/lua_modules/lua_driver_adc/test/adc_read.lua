local adc = require("adc")
local delay = require("delay")

local ADC_GPIO = 4
local SAMPLE_COUNT = 5
local SAMPLE_INTERVAL_MS = 200

local ch = adc.new(ADC_GPIO)

print(string.format(
    "[adc_demo] reading gpio=%d for %d samples...",
    ch:get_gpio(),
    SAMPLE_COUNT
))

for i = 1, SAMPLE_COUNT do
    print(string.format(
        "[adc_demo] sample %d/%d: %d mV",
        i,
        SAMPLE_COUNT,
        ch:read()
    ))

    if i < SAMPLE_COUNT then
        delay.delay_ms(SAMPLE_INTERVAL_MS)
    end
end

ch:close()
print("[adc_demo] done")
