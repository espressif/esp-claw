local display = require("display")
local delay = require("delay")
local options = type(args) == "table" and args or {}

if options.exit_without_close then
    local screen = display.open()
    screen:begin({ clear = 0xFF000000 })
    screen:present()
    print("[display_busy_contract] task exit without close")
    if options.raise_error then error("intentional task error") end
elseif options.expect_busy then
    local ok, err = pcall(display.open)
    assert(not ok and tostring(err):find("already open", 1, true), "cross-task open must be busy")
    print("[display_busy_contract] PASS busy")
else
    local hold_ms = math.tointeger(options.hold_ms or 5000)
    assert(hold_ms and hold_ms > 0 and hold_ms <= 30000)
    do
        local screen <close> = display.open()
        print("[display_busy_contract] holding screen")
        delay.delay_ms(hold_ms)
    end
    print("[display_busy_contract] PASS released")
end
