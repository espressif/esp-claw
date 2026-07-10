-- --------------------------------------------------------------
-- Control a simple GPIO-driven light.
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local gpio = require("gpio")

-- 2. Constants
local DEFAULT_IO = 38
local DEFAULT_ENABLED = true
local DEFAULT_ACTIVE_LEVEL = 1
local DEFAULT_BRIGHTNESS = 255
local MIN_GPIO_NUM = 0
local MIN_LEVEL = 0
local MAX_LEVEL = 1
local MIN_BRIGHTNESS = 0
local MAX_BRIGHTNESS = 255

-- 3. Args
local ARG_SCHEMA = {
  io = arg_schema.int({ default = DEFAULT_IO, min = MIN_GPIO_NUM }),
  enabled = arg_schema.bool({ default = DEFAULT_ENABLED }),
  active_level = arg_schema.int({ default = DEFAULT_ACTIVE_LEVEL, min = MIN_LEVEL, max = MAX_LEVEL }),
  brightness = arg_schema.int({ default = DEFAULT_BRIGHTNESS, min = MIN_BRIGHTNESS, max = MAX_BRIGHTNESS }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)

local function inactive_level()
  return 1 - ctx.active_level
end

local function target_level()
  -- GPIO lights are binary; brightness 0 is treated as off, any non-zero brightness is on.
  if not ctx.enabled or ctx.brightness == 0 then
    return inactive_level()
  end
  return ctx.active_level
end

-- 4. Run
local function run()
  local level = target_level()

  gpio.set_direction(ctx.io, "output")
  gpio.set_level(ctx.io, level)

  if level == ctx.active_level then
    print("[light_switch] gpio light on")
  else
    print("[light_switch] gpio light off")
  end
end

-- 5. Epilogue
local ok, err = xpcall(run, debug.traceback)
if not ok then
  print("[light_switch] ERROR: " .. tostring(err))
  error(err)
end
