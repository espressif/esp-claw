-- --------------------------------------------------------------
-- Read smoothed capacitive touch values from explicit GPIOs.
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local delay = require("delay")
local touch = require("touch")

-- 2. Constants
local DEFAULT_SAMPLES = 10
local DEFAULT_INTERVAL_MS = 200
local MAX_SAMPLES = 100
local MAX_INTERVAL_MS = 10000

-- 3. Args
local ARG_SCHEMA = {
  samples = arg_schema.int({ default = DEFAULT_SAMPLES, min = 1, max = MAX_SAMPLES }),
  interval_ms = arg_schema.int({ default = DEFAULT_INTERVAL_MS, min = 0, max = MAX_INTERVAL_MS }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)

local function parse_gpios()
  if type(args) ~= "table" or type(args.gpios) ~= "table" then
    error("gpios must be a non-empty array of GPIO numbers")
  end

  local gpios = {}
  for i, value in ipairs(args.gpios) do
    if type(value) ~= "number" then
      error("gpios[" .. tostring(i) .. "] must be a number")
    end

    local gpio = math.floor(value)
    if gpio < 0 then
      error("gpios[" .. tostring(i) .. "] must be >= 0")
    end

    for _, existing in ipairs(gpios) do
      if existing == gpio then
        error("gpios must not contain duplicates")
      end
    end

    gpios[#gpios + 1] = gpio
  end

  if #gpios == 0 then
    error("gpios must not be empty")
  end

  return gpios
end

ctx.gpios = parse_gpios()

-- 4. Cleanup
local sensor

local function cleanup()
  if sensor then
    local ok, err = pcall(function()
      sensor:close()
    end)
    if not ok then
      print("[read_touch] WARN: touch close failed: " .. tostring(err))
    end
    sensor = nil
  end
end

-- 5. Run
local function run()
  sensor = touch.new({ gpios = ctx.gpios })
  print("[read_touch] opened " .. sensor:name())

  for sample_index = 1, ctx.samples do
    local sample = sensor:read()

    for _, key in ipairs(sample.keys) do
      print(string.format(
        "[read_touch] sample=%d gpio=%d channel=%d smooth=%d",
        sample_index,
        key.gpio,
        key.channel,
        key.smooth
      ))
    end

    if sample_index < ctx.samples and ctx.interval_ms > 0 then
      delay.delay_ms(ctx.interval_ms)
    end
  end
end

-- 6. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
  print("[read_touch] ERROR: " .. tostring(err))
  error(err)
end
