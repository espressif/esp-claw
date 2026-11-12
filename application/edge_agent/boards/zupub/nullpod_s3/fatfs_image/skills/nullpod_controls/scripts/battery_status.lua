-- --------------------------------------------------------------
-- Read the battery voltage from the GPIO1 divider (56k / 22k).
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local adc = require("adc")
local delay = require("delay")
local json = require("json")

-- 2. Constants
local BATTERY_ADC_GPIO = 1
local DIVIDER_TOP_OHM = 56000
local DIVIDER_BOTTOM_OHM = 22000
local DIVIDER_SCALE = (DIVIDER_TOP_OHM + DIVIDER_BOTTOM_OHM) / DIVIDER_BOTTOM_OHM
local DEFAULT_SAMPLES = 16
local MIN_SAMPLES = 1
local MAX_SAMPLES = 64
local SAMPLE_INTERVAL_MS = 5
local EMPTY_MV = 3300
local FULL_MV = 4200
local LOW_BATTERY_MV = 3400
local PERCENT_MAX = 100
local PERCENT_MIN = 0

-- 3. Args
local ARG_SCHEMA = {
  samples = arg_schema.int({ default = DEFAULT_SAMPLES, min = MIN_SAMPLES, max = MAX_SAMPLES }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)

-- 4. Module-local state
local channel = nil

-- 5. Cleanup
local function cleanup()
  if channel then
    pcall(channel.close, channel)
    channel = nil
  end
end

-- 6. Run
local function run()
  channel = adc.new(BATTERY_ADC_GPIO)

  local sum_mv = 0
  for _ = 1, ctx.samples do
    sum_mv = sum_mv + channel:read()
    delay.delay_ms(SAMPLE_INTERVAL_MS)
  end
  local adc_mv = math.floor(sum_mv / ctx.samples + 0.5)
  local battery_mv = math.floor(adc_mv * DIVIDER_SCALE + 0.5)

  local percent = math.floor((battery_mv - EMPTY_MV) * PERCENT_MAX / (FULL_MV - EMPTY_MV) + 0.5)
  percent = math.max(PERCENT_MIN, math.min(PERCENT_MAX, percent))

  print(json.encode({
    adc_mv = adc_mv,
    battery_mv = battery_mv,
    percent = percent,
    low = battery_mv < LOW_BATTERY_MV,
  }))
end

-- 7. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
  print("[nullpod_controls] ERROR: " .. tostring(err))
  error(err)
end
