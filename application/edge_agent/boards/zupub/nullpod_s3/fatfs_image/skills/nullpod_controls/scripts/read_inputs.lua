-- --------------------------------------------------------------
-- Poll the NullPod five-way keys and rotary encoder.
-- Prints one JSON object with the events seen inside the window.
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local button = require("button")
local knob = require("knob")
local delay = require("delay")
local json = require("json")

-- 2. Constants
local DEFAULT_DURATION_MS = 5000
local MIN_DURATION_MS = 100
local MAX_DURATION_MS = 30000
local POLL_INTERVAL_MS = 10
local MAX_EVENTS = 64
local ACTIVE_LEVEL_LOW = 0

local ENCODER_GPIO_A = 9
local ENCODER_GPIO_B = 3

-- Key names follow the silkscreen: the SEL pin is the joystick centre press.
local KEYS = {
  { name = "up", gpio = 16 },
  { name = "down", gpio = 12 },
  { name = "left", gpio = 21 },
  { name = "right", gpio = 2 },
  { name = "center", gpio = 0 },
}

local KEY_EVENTS = { "single_click", "double_click", "long_press_start" }
local KNOB_EVENTS = { "left", "right" }

-- 3. Args
local ARG_SCHEMA = {
  duration_ms = arg_schema.int({
    default = DEFAULT_DURATION_MS,
    min = MIN_DURATION_MS,
    max = MAX_DURATION_MS,
  }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)

-- 4. Module-local state
local events = {}
local button_handles = {}
local knob_handle = nil

local function record(entry)
  if #events < MAX_EVENTS then
    events[#events + 1] = entry
  end
end

-- 5. Cleanup
local function cleanup()
  for _, handle in ipairs(button_handles) do
    pcall(button.close, handle)
  end
  button_handles = {}
  if knob_handle then
    pcall(knob.close, knob_handle)
    knob_handle = nil
  end
end

-- 6. Run
local function run()
  for _, key in ipairs(KEYS) do
    local handle, err = button.new(key.gpio, ACTIVE_LEVEL_LOW)
    if not handle then
      print("[nullpod_controls] WARN: button.new(" .. key.name .. ") failed: " .. tostring(err))
    else
      button_handles[#button_handles + 1] = handle
      for _, event in ipairs(KEY_EVENTS) do
        button.on(handle, event, function()
          record({ source = "key", key = key.name, event = event })
        end)
      end
    end
  end

  local knob_err = nil
  knob_handle, knob_err = knob.new(ENCODER_GPIO_A, ENCODER_GPIO_B)
  if not knob_handle then
    print("[nullpod_controls] WARN: knob.new failed: " .. tostring(knob_err))
  else
    for _, event in ipairs(KNOB_EVENTS) do
      knob.on(knob_handle, event, function(evt)
        record({ source = "encoder", event = event, count = evt and evt.count or 0 })
      end)
    end
  end

  -- Both modules are poll-based: dispatch() drains the driver queue, then we
  -- sleep one slot so the next sample sees fresh pin levels.
  local elapsed_ms = 0
  while elapsed_ms < ctx.duration_ms do
    button.dispatch()
    if knob_handle then
      knob.dispatch()
    end
    delay.delay_ms(POLL_INTERVAL_MS)
    elapsed_ms = elapsed_ms + POLL_INTERVAL_MS
  end

  -- An empty Lua table is ambiguous between array and object, so json.encode
  -- would emit "events":{} for a quiet window. Serialise the array by hand to
  -- keep the contract "events": [...] in every case.
  local encoded_events = {}
  for index = 1, #events do
    encoded_events[index] = json.encode(events[index])
  end

  print(string.format(
    '{"duration_ms":%d,"events":[%s]}',
    ctx.duration_ms,
    table.concat(encoded_events, ",")
  ))
end

-- 7. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
  print("[nullpod_controls] ERROR: " .. tostring(err))
  error(err)
end
