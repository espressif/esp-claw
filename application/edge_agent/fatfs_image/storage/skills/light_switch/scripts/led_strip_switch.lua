-- --------------------------------------------------------------
-- Light LED strip pixels with a configurable color.
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local led_strip = require("led_strip")

-- 2. Constants
local DEFAULT_IO = 38
local DEFAULT_LED_COUNT = 1
local DEFAULT_ENABLED = true
local DEFAULT_BRIGHTNESS = 255
local MIN_GPIO_NUM = 0
local MIN_BRIGHTNESS = 0
local MAX_BRIGHTNESS = 255
local MIN_COLOR_CHANNEL = 0
local MAX_COLOR_CHANNEL = 255
local DEFAULT_COLOR = { r = 255, g = 255, b = 255 }
local PIXEL_INDEX = 0

-- 3. Args
local ARG_SCHEMA = {
  io = arg_schema.int({ default = DEFAULT_IO, min = MIN_GPIO_NUM }),
  led_count = arg_schema.int({ default = DEFAULT_LED_COUNT, min = 1 }),
  enabled = arg_schema.bool({ default = DEFAULT_ENABLED }),
  brightness = arg_schema.int({ default = DEFAULT_BRIGHTNESS, min = MIN_BRIGHTNESS, max = MAX_BRIGHTNESS }),
  color = arg_schema.object({
    default = DEFAULT_COLOR,
    fields = {
      r = arg_schema.int({ default = DEFAULT_COLOR.r, min = MIN_COLOR_CHANNEL, max = MAX_COLOR_CHANNEL }),
      g = arg_schema.int({ default = DEFAULT_COLOR.g, min = MIN_COLOR_CHANNEL, max = MAX_COLOR_CHANNEL }),
      b = arg_schema.int({ default = DEFAULT_COLOR.b, min = MIN_COLOR_CHANNEL, max = MAX_COLOR_CHANNEL }),
    },
  }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)

-- 4. Module-local state
local strip = nil

-- 5. Cleanup
local function cleanup()
  if strip then
    pcall(strip.close, strip)
    strip = nil
  end
end

local function scale_channel(channel, brightness)
  return math.floor(channel * brightness / MAX_BRIGHTNESS)
end

local function apply_color(r, g, b)
  -- Apply the same color to every pixel so multi-pixel strips respond as one light.
  for index = PIXEL_INDEX, ctx.led_count - 1 do
    strip:set_pixel(index, r, g, b)
  end
  strip:refresh()
end

-- 6. Run
local function run()
  local new_strip, strip_err = led_strip.new(ctx.io, ctx.led_count)
  if not new_strip then
    print("[light_switch] ERROR: led_strip.new failed: " .. tostring(strip_err))
    error("led_strip.new failed: " .. tostring(strip_err))
  end
  strip = new_strip

  if not ctx.enabled then
    strip:clear()
    strip:refresh()
    print("[light_switch] led strip off")
    return
  end

  apply_color(
    scale_channel(ctx.color.r, ctx.brightness),
    scale_channel(ctx.color.g, ctx.brightness),
    scale_channel(ctx.color.b, ctx.brightness)
  )
  print("[light_switch] led strip on")
end

-- 7. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
  print("[light_switch] ERROR: " .. tostring(err))
  error(err)
end
