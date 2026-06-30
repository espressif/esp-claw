-- ──────────────────────────────────────────────────────────────
-- Flash an LED strip, then play a rainbow sweep for a short demo.
-- ──────────────────────────────────────────────────────────────

-- 1. Requires
local delay = require("delay")
local led_strip = require("led_strip")

-- 2. Constants
local DEFAULT_IO = 38
local LED_COUNT = 16
local FLASH_MS = 150
local RAINBOW_STEP_MS = 40
local FLASH_REPEAT_COUNT = 3
local HUE_RANGE = 360
local RAINBOW_CYCLE_DEGREES = 720
local RAINBOW_STEP_DEGREES = 8
local MAX_SATURATION = 255
local RAINBOW_VALUE = 64
local FULL_BRIGHTNESS = 255
local LED_OFF = 0

-- 3. Module-local state
local strip = nil

-- 4. Cleanup
local function cleanup()
  if strip then
    pcall(strip.clear, strip)
    pcall(strip.refresh, strip)
    pcall(strip.close, strip)
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
    local hue = ((index * HUE_RANGE) // LED_COUNT + offset) % HUE_RANGE
    strip:set_pixel_hsv(index, hue, MAX_SATURATION, RAINBOW_VALUE)
  end
end

-- 5. Run
local function run()
  print("[basic_led_strip] init io=" .. tostring(DEFAULT_IO) .. " leds=" .. tostring(LED_COUNT))
  strip = led_strip.new(DEFAULT_IO, LED_COUNT)

  strip:clear()
  strip:refresh()
  print("[basic_led_strip] flash animation")

  for _ = 1, FLASH_REPEAT_COUNT do
    fill_all_rgb(FULL_BRIGHTNESS, FULL_BRIGHTNESS, FULL_BRIGHTNESS)
    strip:refresh()
    delay.delay_ms(FLASH_MS)

    strip:clear()
    strip:refresh()
    delay.delay_ms(FLASH_MS)
  end

  print("[basic_led_strip] rainbow animation")
  for offset = 0, RAINBOW_CYCLE_DEGREES, RAINBOW_STEP_DEGREES do
    draw_rainbow(offset)
    strip:refresh()
    delay.delay_ms(RAINBOW_STEP_MS)
  end

  fill_all_rgb(LED_OFF, LED_OFF, LED_OFF)
  strip:refresh()
  print("[basic_led_strip] done")
end

-- 6. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then error(err) end
