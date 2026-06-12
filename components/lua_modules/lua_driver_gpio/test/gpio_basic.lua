-- GPIO demo: configure a pin, drive output levels, read them back, then disable it.
local gpio = require("gpio")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local TEST_GPIO = type(a.gpio) == "number" and math.floor(a.gpio) or 4
local BLINK_COUNT = type(a.count) == "number" and math.floor(a.count) or 3
local INTERVAL_MS = type(a.interval_ms) == "number" and math.floor(a.interval_ms) or 500

local function set_and_read(level)
    gpio.set_level(TEST_GPIO, level)
    delay.delay_ms(INTERVAL_MS)

    local actual = gpio.get_level(TEST_GPIO)
    print(string.format(
        "[gpio_demo] gpio=%d set=%d read=%d",
        TEST_GPIO,
        level,
        actual
    ))
end

print(string.format("[gpio_demo] using gpio=%d", TEST_GPIO))

gpio.set_direction(TEST_GPIO, "output")
print(string.format("[gpio_demo] gpio=%d direction=output", TEST_GPIO))

for _ = 1, BLINK_COUNT do
    set_and_read(1)
    set_and_read(0)
end

gpio.set_direction(TEST_GPIO, "input_output")
print(string.format("[gpio_demo] gpio=%d direction=input_output", TEST_GPIO))
set_and_read(1)
set_and_read(0)

gpio.set_direction(TEST_GPIO, "input")
print(string.format(
    "[gpio_demo] gpio=%d direction=input read=%d",
    TEST_GPIO,
    gpio.get_level(TEST_GPIO)
))

gpio.set_direction(TEST_GPIO, "disable")
print(string.format("[gpio_demo] gpio=%d direction=disable", TEST_GPIO))
print("[gpio_demo] done")
