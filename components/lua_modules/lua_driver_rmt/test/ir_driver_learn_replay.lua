-- IR helper demo built on lua_driver_rmt/lib/ir_driver.lua.
-- Defaults match ESP32-S3-BOX-3 style wiring.
local ir_driver = require("ir_driver")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local TX_GPIO = type(a.tx_gpio) == "number" and math.floor(a.tx_gpio) or 39
local RX_GPIO = type(a.rx_gpio) == "number" and math.floor(a.rx_gpio) or 38
local CTRL_GPIO = type(a.ctrl_gpio) == "number" and math.floor(a.ctrl_gpio) or 44
local CARRIER_HZ = type(a.carrier_hz) == "number" and math.floor(a.carrier_hz) or 38000
local LEARN_MS = type(a.learn_ms) == "number" and math.floor(a.learn_ms) or 5000
local ADDRESS = type(a.address) == "number" and math.floor(a.address) or 0x00FF
local COMMAND = type(a.command) == "number" and math.floor(a.command) or 0x10EF

local dev

local function cleanup()
    if dev then
        pcall(function() dev:close() end)
        dev = nil
    end
end

local function run()
    dev = ir_driver.new({
        name = "ir_driver_demo",
        tx_gpio = TX_GPIO,
        rx_gpio = RX_GPIO,
        ctrl_gpio = CTRL_GPIO,
        carrier_hz = CARRIER_HZ,
    })

    print(string.format("[ir_driver] tx=%d rx=%d ctrl=%d carrier=%dHz",
        TX_GPIO, RX_GPIO, CTRL_GPIO, CARRIER_HZ))
    print(string.format("[ir_driver] press a remote key (timeout %dms)...", LEARN_MS))

    local symbols, err = dev:receive(LEARN_MS)
    if not symbols then
        print("[ir_driver] receive failed: " .. tostring(err))
        print(string.format("[ir_driver] fallback NEC addr=0x%04X cmd=0x%04X", ADDRESS, COMMAND))
        dev:send_nec(ADDRESS, COMMAND)
        return
    end

    print(string.format("[ir_driver] learned %d symbols", #symbols))
    local decoded = ir_driver.decode_nec(symbols)
    if decoded then
        print(string.format("[ir_driver] NEC addr=0x%04X cmd=0x%04X",
            decoded.address, decoded.command))
    end

    delay.delay_ms(500)
    print("[ir_driver] replay learned frame")
    dev:send_raw(symbols)
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
