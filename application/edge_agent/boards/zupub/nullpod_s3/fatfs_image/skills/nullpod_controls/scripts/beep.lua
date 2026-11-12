-- --------------------------------------------------------------
-- Beep the NullPod passive buzzer on GPIO46.
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local mcpwm = require("mcpwm")
local delay = require("delay")

-- 2. Constants
local BUZZER_GPIO = 46
local DEFAULT_FREQUENCY_HZ = 2700
local MIN_FREQUENCY_HZ = 100
local MAX_FREQUENCY_HZ = 10000
local DEFAULT_DURATION_MS = 120
local MIN_DURATION_MS = 10
local MAX_DURATION_MS = 2000
local DEFAULT_DUTY_PERCENT = 50
local MIN_DUTY_PERCENT = 1
local MAX_DUTY_PERCENT = 100

-- 3. Args
local ARG_SCHEMA = {
  frequency_hz = arg_schema.int({
    default = DEFAULT_FREQUENCY_HZ,
    min = MIN_FREQUENCY_HZ,
    max = MAX_FREQUENCY_HZ,
  }),
  duration_ms = arg_schema.int({
    default = DEFAULT_DURATION_MS,
    min = MIN_DURATION_MS,
    max = MAX_DURATION_MS,
  }),
  duty_percent = arg_schema.int({
    default = DEFAULT_DUTY_PERCENT,
    min = MIN_DUTY_PERCENT,
    max = MAX_DUTY_PERCENT,
  }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)

-- 4. Module-local state
local pwm = nil

-- 5. Cleanup
local function cleanup()
  if pwm then
    pcall(pwm.stop, pwm)
    pcall(pwm.close, pwm)
    pwm = nil
  end
end

-- 6. Run
local function run()
  local handle, err = mcpwm.new({
    gpio = BUZZER_GPIO,
    frequency_hz = ctx.frequency_hz,
    duty_percent = ctx.duty_percent,
  })
  if not handle then
    print("[nullpod_controls] ERROR: mcpwm.new failed: " .. tostring(err))
    error("mcpwm.new failed: " .. tostring(err))
  end
  pwm = handle

  pwm:start()
  delay.delay_ms(ctx.duration_ms)
  pwm:stop()
  print("[nullpod_controls] beep " .. ctx.frequency_hz .. " Hz for " .. ctx.duration_ms .. " ms")
end

-- 7. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
  print("[nullpod_controls] ERROR: " .. tostring(err))
  error(err)
end
