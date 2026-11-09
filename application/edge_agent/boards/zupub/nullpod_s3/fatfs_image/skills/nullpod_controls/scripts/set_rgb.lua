-- --------------------------------------------------------------
-- Drive the single WS2812 RGB pixel on GPIO45.
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local led_strip = require("led_strip")

-- 2. Constants
local RGB_GPIO = 45
local RGB_COUNT = 1
local MAX_CHANNEL = 255
local MIN_CHANNEL = 0
local DEFAULT_BRIGHTNESS = 128
local DEFAULT_COLOR = { r = 255, g = 255, b = 255 }

-- 3. Args
local ARG_SCHEMA = {
  enabled = arg_schema.bool({ default = true }),
  brightness = arg_schema.int({ default = DEFAULT_BRIGHTNESS, min = MIN_CHANNEL, max = MAX_CHANNEL }),
  color = arg_schema.object({
    default = DEFAULT_COLOR,
    fields = {
      r = arg_schema.int({ default = DEFAULT_COLOR.r, min = MIN_CHANNEL, max = MAX_CHANNEL }),
      g = arg_schema.int({ default = DEFAULT_COLOR.g, min = MIN_CHANNEL, max = MAX_CHANNEL }),
      b = arg_schema.int({ default = DEFAULT_COLOR.b, min = MIN_CHANNEL, max = MAX_CHANNEL }),
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

local function scale_channel(channel)
  return math.floor(channel * ctx.brightness / MAX_CHANNEL)
end

-- 6. Run
local function run()
  local handle, err = led_strip.new(RGB_GPIO, RGB_COUNT)
  if not handle then
    print("[nullpod_controls] ERROR: led_strip.new failed: " .. tostring(err))
    error("led_strip.new failed: " .. tostring(err))
  end
  strip = handle

  if not ctx.enabled or ctx.brightness == 0 then
    strip:clear()
    strip:refresh()
    print("[nullpod_controls] rgb off")
    return
  end

  local r = scale_channel(ctx.color.r)
  local g = scale_channel(ctx.color.g)
  local b = scale_channel(ctx.color.b)
  strip:set_pixel(0, r, g, b)
  strip:refresh()
  print("[nullpod_controls] rgb set to " .. r .. "," .. g .. "," .. b)
end

-- 7. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
  print("[nullpod_controls] ERROR: " .. tostring(err))
  error(err)
end
