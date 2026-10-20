-- Probe SCI addresses on one initialized board bus; optional args.port defaults to 0.
local sci = require("sci")
local i2c = require("i2c")
local a = type(args) == "table" and args or {}
local bus = i2c.borrow(a.port or 0)
local found = false
local ok, err = pcall(function()
    for _, address in ipairs({0x21, 0x22, 0x23}) do
        local dev
        local opened, result = pcall(function()
            dev = sci.new({bus = bus, address = address, timeout_ms = 1200})
            return dev:get_version().text
        end)
        if dev then dev:close() end
        print(string.format("0x%02X: %s", address, tostring(result)))
        if opened then found = true; break end
    end
end)
bus:close()
assert(ok, err)
assert(found, "SCI module not found on the selected bus")
