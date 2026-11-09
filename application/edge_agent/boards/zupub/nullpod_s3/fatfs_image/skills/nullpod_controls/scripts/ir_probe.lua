-- --------------------------------------------------------------
-- Fire NEC frames from the NullPod IR blaster on GPIO42 (TX only).
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local ir = require("ir")
local delay = require("delay")

-- 2. Constants
local DEFAULT_ADDRESS = 0x0004
local DEFAULT_COMMAND = 0x0008
local CODE_MIN = 0
local CODE_MAX = 0xFFFF
local DEFAULT_REPEAT_COUNT = 1
local MIN_REPEAT_COUNT = 1
local MAX_REPEAT_COUNT = 4
local DEFAULT_GAP_MS = 120
local MIN_GAP_MS = 40
local MAX_GAP_MS = 2000

-- 3. Args
local ARG_SCHEMA = {
  address = arg_schema.int({
    default = DEFAULT_ADDRESS,
    min = CODE_MIN,
    max = CODE_MAX,
  }),
  command = arg_schema.int({
    default = DEFAULT_COMMAND,
    min = CODE_MIN,
    max = CODE_MAX,
  }),
  repeat_count = arg_schema.int({
    default = DEFAULT_REPEAT_COUNT,
    min = MIN_REPEAT_COUNT,
    max = MAX_REPEAT_COUNT,
  }),
  gap_ms = arg_schema.int({
    default = DEFAULT_GAP_MS,
    min = MIN_GAP_MS,
    max = MAX_GAP_MS,
  }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)

-- 4. Module-local state
local blaster = nil

-- 5. Cleanup
local function cleanup()
  if blaster then
    pcall(blaster.close, blaster)
    blaster = nil
  end
end

-- 6. Run
local function run()
  local handle, err = ir.new()
  if not handle then
    print("[nullpod_controls] ERROR: ir.new failed: " .. tostring(err))
    error("ir.new failed: " .. tostring(err))
  end
  blaster = handle

  local info = blaster:info()
  print(string.format(
    "[nullpod_controls] ir tx=%d rx=%d ctrl=%d carrier=%dHz",
    info.tx_gpio, info.rx_gpio, info.ctrl_gpio, info.carrier_hz
  ))

  for index = 1, ctx.repeat_count do
    blaster:send_nec(ctx.address, ctx.command)
    print(string.format(
      "[nullpod_controls] nec #%d addr=0x%04X cmd=0x%04X",
      index, ctx.address, ctx.command
    ))
    if index < ctx.repeat_count then
      delay.delay_ms(ctx.gap_ms)
    end
  end

  print(string.format(
    "[nullpod_controls] sent %d NEC frame(s); point a phone camera at the IR LED to confirm",
    ctx.repeat_count
  ))
end

-- 7. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
  print("[nullpod_controls] ERROR: " .. tostring(err))
  error(err)
end
