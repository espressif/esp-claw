local sci = require("sci")

local I2C_PORT = 1
local SDA_GPIO = 47
local SCL_GPIO = 48
local SCI_ADDR = 0x21

local dev = sci.new(I2C_PORT, SDA_GPIO, SCL_GPIO, SCI_ADDR, 100000)
dev:set_refresh_rate(sci.REFRESH_1S)

local version = dev:get_version()
print("SCI firmware: " .. version.text)
print("SCI address: 0x" .. string.format("%02X", dev:get_address()))
print("SKUs: " .. dev:get_sku(sci.ALL))
print("Keys: " .. dev:get_keys(sci.ALL))
print("Values: " .. dev:get_values(sci.ALL))
print("Units: " .. dev:get_units(sci.ALL))
print("Information: " .. dev:get_information(sci.ALL, true))

dev:close()
