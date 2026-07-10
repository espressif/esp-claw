-- RMT raw TX/RX smoke test. Wire tx_gpio to rx_gpio for a loopback test.
local rmt = require("rmt")

local a = type(args) == "table" and args or {}
local TX_GPIO = type(a.tx_gpio) == "number" and math.floor(a.tx_gpio) or 39
local RX_GPIO = type(a.rx_gpio) == "number" and math.floor(a.rx_gpio) or 38
local TIMEOUT_MS = type(a.timeout_ms) == "number" and math.floor(a.timeout_ms) or 1000

local symbols = {
    { level0 = 1, duration0 = 1000, level1 = 0, duration1 = 1000 },
    { level0 = 1, duration0 = 500,  level1 = 0, duration1 = 1500 },
    { level0 = 1, duration0 = 1500, level1 = 0, duration1 = 500 },
}

local tx
local rx

local function cleanup()
    if tx then
        pcall(function() tx:close() end)
        tx = nil
    end
    if rx then
        pcall(function() rx:close() end)
        rx = nil
    end
end

local function run()
    tx = rmt.tx({ gpio = TX_GPIO })
    rx = rmt.rx({ gpio = RX_GPIO })

    print(string.format("[rmt] loopback tx=%d rx=%d", TX_GPIO, RX_GPIO))
    rx:start()
    tx:send(symbols)

    local got, err = rx:read(TIMEOUT_MS)
    if not got then
        error("[rmt] receive failed: " .. tostring(err))
    end

    print(string.format("[rmt] received %d symbols", #got))
    for i = 1, #got do
        local s = got[i]
        print(string.format("  [%02d] L%d %d / L%d %d",
            i, s.level0, s.duration0, s.level1, s.duration1))
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
