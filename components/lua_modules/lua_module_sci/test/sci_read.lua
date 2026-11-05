-- Use an initialized board bus. Optional args: port=0, address=0x21.
local sci = require("sci")
local i2c = require("i2c")
local a = type(args) == "table" and args or {}
local bus = i2c.borrow(a.port or 0)
local dev
local ok, err = xpcall(function()
    dev = sci.new({bus = bus, address = a.address or sci.DEFAULT_ADDR})
    assert(dev:set_refresh_rate(sci.REFRESH_1S) == 0, "refresh-rate command failed")
    print("SCI firmware: " .. dev:get_version().text)
    print("SCI address: " .. dev:get_address())
    print("SKUs: " .. dev:get_sku(sci.ALL))
    print("Keys: " .. dev:get_keys(sci.ALL))
    print("Values: " .. dev:get_values(sci.ALL))
    print("Units: " .. dev:get_units(sci.ALL))
    print("Information: " .. dev:get_information(sci.ALL, true))
end, debug.traceback)
if dev then dev:close() end
bus:close()
assert(ok, err)
