-- Run from the repository root with Lua 5.4 or newer. No hardware is required.
-- Exercise the real monitor scripts against the current screen-object API.
local root = "components/lua_modules/"

local function check_monitor(kind, failure)
    local state = { frames = 0, texts = {}, sends = 0, reads = 0, closed = 0, screen_closed = 0 }
    local frame_active = false
    local screen = {}
    function screen:info() return { width = 480, height = 320 } end
    function screen:begin(options)
        assert(not frame_active and type(options.clear) == "string")
        frame_active = true
    end
    function screen:measure_text(text, options) return #text * options.font_size // 2, options.font_size end
    function screen:text(x, y, text)
        assert(frame_active and x >= 0 and y >= 0)
        state.texts[#state.texts + 1] = text
    end
    function screen:present()
        assert(frame_active)
        frame_active = false
        state.frames = state.frames + 1
    end
    function screen:close() state.screen_closed = state.screen_closed + 1 end

    local device = {}
    function device:send(...)
        state.sends = state.sends + 1
        return true
    end
    function device:receive()
        state.reads = state.reads + 1
        if state.reads > 1 then error("TEST_STOP") end
        if kind == "can" then return { data = { 0, 0, 0, 0, 1, 2, 3, 4 }, dlc = 8 } end
        return { data = "hello", length = 5, rssi = -70.5, snr = 8.0 }
    end
    function device:status() return { state = "running" } end
    function device:start_receive() return true end
    function device:close() state.closed = state.closed + 1 end

    local modules = {
        display = { open = function()
            if failure == "screen" then error("TEST_STOP screen unavailable") end
            return screen
        end },
        system = { millis = function() return 100 end },
        delay = { delay_ms = function() error("TEST_STOP") end },
    }
    local driver = { new = function()
        if failure == "radio" then error("sx1262 init failed") end
        return device
    end }
    modules.twai, modules.sx1262 = driver, driver
    local env = setmetatable({
        args = { tx_gpio = 1, rx_gpio = 2, frequency_hz = 868000000, send_text = "ping" },
        print = function() end,
        require = function(name) return assert(modules[name], "unexpected dependency: " .. name) end,
    }, { __index = _G })
    local path = kind == "can"
        and root .. "lua_driver_twai/skills/can_bus/scripts/can_lcd_monitor.lua"
        or root .. "lua_driver_sx1262/skills/lora_radio/scripts/lora_lcd_monitor.lua"
    local ok, err = pcall(assert(loadfile(path, "t", env)))
    assert(not ok and tostring(err):find("TEST_STOP", 1, true), tostring(err))
    assert(state.screen_closed == (failure == "screen" and 0 or 1), "screen cleanup")
    assert(state.closed == (failure == "radio" and 0 or 1), "device cleanup")
    if not failure then
        assert(state.sends == 1 and state.reads == 2, "TX/RX loop")
        assert(state.frames >= 2 and not frame_active, "complete display frames")
        local texts = table.concat(state.texts, "\n")
        assert(texts:find(kind == "can" and "RX:01020304" or "RX: hello", 1, true), "RX displayed")
    elseif failure == "radio" then
        assert(table.concat(state.texts, "\n"):find("INIT ERROR", 1, true), "init error displayed")
    end
end

check_monitor("can")
check_monitor("can", "screen")
check_monitor("lora")
check_monitor("lora", "radio")
print("PASS: CAN/LoRa display rendering, TX/RX, and failure cleanup (4 scenarios)")
