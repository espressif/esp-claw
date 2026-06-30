-- Basic IR demo: learn one IR frame from a remote, then replay it.
-- Defaults match ESP32-S3-BOX-3 (BSP_IR_TX_GPIO=39, BSP_IR_RX_GPIO=38, BSP_IR_CTRL_GPIO=44).
local ir = require("ir")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local DEVICE_NAME = type(a.device) == "string" and a.device or "ir_blaster"
local OPTS = {
    tx_gpio    = type(a.tx_gpio) == "number"    and math.floor(a.tx_gpio)    or 39,
    rx_gpio    = type(a.rx_gpio) == "number"    and math.floor(a.rx_gpio)    or 38,
    ctrl_gpio  = type(a.ctrl_gpio) == "number"  and math.floor(a.ctrl_gpio)  or 44,
    carrier_hz = type(a.carrier_hz) == "number" and math.floor(a.carrier_hz) or 38000,
}
local ADDRESS    = type(a.address) == "number" and math.floor(a.address) or 0x00FF
local COMMAND    = type(a.command) == "number" and math.floor(a.command) or 0x10EF
local LEARN_MS   = type(a.learn_ms) == "number" and math.floor(a.learn_ms) or 5000

local dev

local function cleanup()
    if dev then
        pcall(function() dev:close() end)
        dev = nil
    end
end

local function run()
    dev = ir.new(DEVICE_NAME, OPTS)
    print("[ir] opened " .. dev:name())
    local info = dev:info()
    print(string.format("[ir] tx=%d rx=%d ctrl=%d carrier=%dHz",
        info.tx_gpio, info.rx_gpio, info.ctrl_gpio, info.carrier_hz))

    print(string.format("[ir] please press a remote key (timeout %dms)...", LEARN_MS))
    local symbols, err = dev:receive(LEARN_MS)
    if not symbols then
        print("[ir] receive failed: " .. tostring(err))
        print(string.format("[ir] fallback: send NEC addr=0x%04X cmd=0x%04X", ADDRESS, COMMAND))
        dev:send_nec(ADDRESS, COMMAND)
        return
    end

    print(string.format("[ir] learned %d symbols", #symbols))
    for i = 1, math.min(#symbols, 8) do
        local s = symbols[i]
        print(string.format("  [%02d] L%d %dus / L%d %dus",
            i, s.level0, s.duration0, s.level1, s.duration1))
    end
    if #symbols > 8 then
        print(string.format("  ... and %d more", #symbols - 8))
    end

    delay.delay_ms(500)
    print("[ir] replay learned frame")
    dev:send_raw(symbols)
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
