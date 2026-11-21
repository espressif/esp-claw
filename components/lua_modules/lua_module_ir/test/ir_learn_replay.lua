-- Learn one protocol-independent IR frame and replay it.
local ir = require("ir")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local dev

local function option(name, default)
    return type(a[name]) == "number" and math.floor(a[name]) or default
end

local function run()
    dev = ir.device({
        name = "ir_learn_replay",
        tx_gpio = option("tx_gpio", 39),
        rx_gpio = option("rx_gpio", 38),
        ctrl_gpio = option("ctrl_gpio", 44),
        carrier_hz = option("carrier_hz", 38000),
    })

    local timeout_ms = option("learn_ms", 5000)
    print(string.format("[ir] press a remote key within %dms", timeout_ms))
    local frame, err = dev:receive({timeout_ms = timeout_ms})
    if not frame then
        error("IR receive failed: " .. tostring(err))
    end

    print(string.format("[ir] learned %d pulses", #frame))
    delay.delay_ms(500)
    dev:send(frame)
end

local ok, err = xpcall(run, debug.traceback)
if dev then pcall(function() dev:close() end) end
if not ok then error(err) end
