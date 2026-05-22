-- WS2812 demo: flash + rainbow. Optional args: pin, num, flash_ms, rainbow_step_ms (integers).
-- Pattern: xpcall(run) + cleanup() (strip:clear/close); no bare gpio constants without args fallback.
local ls    = require("led_strip")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local LED_GPIO_NUM   = int_arg("pin", 38)
local LED_COUNT      = int_arg("num", 16)
local FLASH_MS       = int_arg("flash_ms", 150)
local RAINBOW_STEP_MS = int_arg("rainbow_step_ms", 40)

local strip

local function cleanup()
    if strip then
        pcall(function()
            strip:clear()
            strip:refresh()
        end)
        pcall(function()
            strip:close()
        end)
        strip = nil
    end
end

local function fill_all_rgb(r, g, b)
    for index = 0, LED_COUNT - 1 do
        strip:set_pixel(index, r, g, b)
    end
end

local function draw_rainbow(offset)
    for index = 0, LED_COUNT - 1 do
        local hue = ((index * 360) // LED_COUNT + offset) % 360
        strip:set_pixel_hsv(index, hue, 255, 64)
    end
end

local function run()
    print("[led] creating led strip on gpio " .. tostring(LED_GPIO_NUM) .. " count=" .. tostring(LED_COUNT))
    strip = ls.new(LED_GPIO_NUM, LED_COUNT)
    print("[led] led strip created")

    strip:clear()
    print("[led] strip cleared")

    print("[led] flash start")
    for i = 1, 3 do
        fill_all_rgb(255, 255, 255)
        strip:refresh()
        delay.delay_ms(FLASH_MS)

        strip:clear()
        strip:refresh()
        delay.delay_ms(FLASH_MS)
    end
    print("[led] flash end")

    print("[led] rainbow animation start")
    for offset = 0, 720, 8 do
        draw_rainbow(offset)
        strip:refresh()
        delay.delay_ms(RAINBOW_STEP_MS)
    end

    print("[led] rainbow animation end")
    strip:clear()
    strip:refresh()
    print("[led] done")
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
