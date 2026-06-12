-- I2C API coverage test. It scans the bus, checks argument validation, and
-- exercises every bus/device method when a target device is present.
local i2c = require("i2c")

local a = type(args) == "table" and args or {}
local I2C_PORT = type(a.port) == "number" and math.floor(a.port) or 0
local SDA_GPIO = type(a.sda) == "number" and math.floor(a.sda) or 14
local SCL_GPIO = type(a.scl) == "number" and math.floor(a.scl) or 13
local FREQ_HZ = type(a.freq_hz) == "number" and math.floor(a.freq_hz) or 400000
local DEVICE_FREQ_HZ = type(a.device_freq_hz) == "number" and math.floor(a.device_freq_hz) or 0

local TARGET_ADDR = type(a.addr) == "number" and math.floor(a.addr) or nil
local START_REG = type(a.reg) == "number" and math.floor(a.reg) or 0x00
local STRING_REG = type(a.string_reg) == "number" and math.floor(a.string_reg) or START_REG
local BYTE_REG = type(a.byte_reg) == "number" and math.floor(a.byte_reg) or START_REG

local TABLE_DATA = type(a.table_data) == "table" and a.table_data or {0x12, 0x34, 0x56, 0x78}
local STRING_DATA = type(a.string_data) == "string" and a.string_data or string.char(0xA5, 0x5A)
local BYTE_DATA = type(a.byte_data) == "number" and math.floor(a.byte_data) or 0x3C

local function bytes_to_string(bytes)
    local chars = {}
    for i = 1, #bytes do
        chars[i] = string.char(bytes[i])
    end
    return table.concat(chars)
end

local function format_bytes(data)
    local parts = {}
    for i = 1, #data do
        parts[i] = string.format("0x%02X", string.byte(data, i))
    end
    return table.concat(parts, " ")
end

local function expect_error(name, fn)
    local ok, err = pcall(fn)
    if ok then
        error("[i2c_test] expected error: " .. name)
    end
    print(string.format("[i2c_test] expected error OK: %s (%s)", name, tostring(err)))
end

local function try_optional(name, fn)
    local ok, value = pcall(fn)
    if ok then
        print(string.format("[i2c_test] %s OK: %s", name, tostring(value)))
    else
        print(string.format("[i2c_test] %s not supported by target: %s", name, tostring(value)))
    end
    return ok, value
end

local function assert_equal(name, actual, expected)
    if actual ~= expected then
        error(string.format(
            "[i2c_test] %s failed: expected=%s actual=%s",
            name,
            tostring(expected),
            tostring(actual)
        ))
    end
    print(string.format("[i2c_test] %s OK", name))
end

local function check_table_bytes(name, bytes)
    for i = 1, #bytes do
        local byte = bytes[i]
        if type(byte) ~= "number" or byte < 0 or byte > 255 or math.floor(byte) ~= byte then
            error(string.format("[i2c_test] %s byte #%d must be an integer in 0-255", name, i))
        end
    end
end

check_table_bytes("table_data", TABLE_DATA)
if BYTE_DATA < 0 or BYTE_DATA > 255 then
    error("[i2c_test] byte_data must be in range 0-255")
end

print(string.format(
    "[i2c_test] covering I2C port=%d sda=%d scl=%d freq=%d",
    I2C_PORT,
    SDA_GPIO,
    SCL_GPIO,
    FREQ_HZ
))

expect_error("i2c.new invalid freq", function()
    i2c.new(I2C_PORT, SDA_GPIO, SCL_GPIO, 0)
end)

local default_bus = i2c.new(I2C_PORT, SDA_GPIO, SCL_GPIO)
print("[i2c_test] i2c.new default frequency OK")
default_bus:close()
print("[i2c_test] default bus close OK")

local bus = i2c.new(I2C_PORT, SDA_GPIO, SCL_GPIO, FREQ_HZ)
print("[i2c_test] i2c.new explicit frequency OK")

expect_error("bus:device invalid address low", function()
    bus:device(-1)
end)
expect_error("bus:device invalid address high", function()
    bus:device(0x80)
end)

local addrs = bus:scan()
print(string.format("[i2c_test] bus:scan found %d device(s)", #addrs))
for _, addr in ipairs(addrs) do
    print(string.format("[i2c_test] found device at 0x%02X", addr))
end

if TARGET_ADDR == nil and #addrs > 0 then
    TARGET_ADDR = addrs[1]
end

if TARGET_ADDR == nil then
    print("[i2c_test] no I2C device found; device API coverage skipped")
    bus:close()
    print("[i2c_test] bus:close OK")
    bus:close()
    print("[i2c_test] bus:close idempotent OK")
    expect_error("closed bus access", function()
        bus:scan()
    end)
    print("[i2c_test] done")
    return
end

local dev = bus:device(TARGET_ADDR, DEVICE_FREQ_HZ)
assert_equal("dev:address", dev:address(), TARGET_ADDR)

expect_error("dev:read invalid length zero", function()
    dev:read(0, START_REG)
end)
expect_error("dev:read invalid length high", function()
    dev:read(1025, START_REG)
end)
expect_error("dev:read invalid mem_addr", function()
    dev:read(1, 0x100)
end)
expect_error("dev:write invalid type", function()
    dev:write(1, START_REG)
end)
expect_error("dev:write invalid table byte", function()
    dev:write({0x00, 0x100}, START_REG)
end)
expect_error("dev:write_byte invalid byte", function()
    dev:write_byte(0x100, BYTE_REG)
end)

local before = dev:read(#TABLE_DATA, START_REG)
print(string.format(
    "[i2c_test] dev:read before addr=0x%02X reg=0x%02X len=%d: %s",
    TARGET_ADDR,
    START_REG,
    #TABLE_DATA,
    format_bytes(before)
))

dev:write(TABLE_DATA, START_REG)
local table_after = dev:read(#TABLE_DATA, START_REG)
assert_equal("dev:write table + dev:read", table_after, bytes_to_string(TABLE_DATA))
print(string.format("[i2c_test] table readback: %s", format_bytes(table_after)))

dev:write(STRING_DATA, STRING_REG)
local string_after = dev:read(#STRING_DATA, STRING_REG)
assert_equal("dev:write string + dev:read", string_after, STRING_DATA)
print(string.format("[i2c_test] string readback: %s", format_bytes(string_after)))

dev:write_byte(BYTE_DATA, BYTE_REG)
local byte_after = dev:read_byte(BYTE_REG)
assert_equal("dev:write_byte + dev:read_byte", byte_after, BYTE_DATA)

try_optional("dev:read_byte without mem_addr", function()
    return string.format("0x%02X", dev:read_byte())
end)
try_optional("dev:read without mem_addr", function()
    return format_bytes(dev:read(1))
end)

dev:write("")
print("[i2c_test] dev:write empty string OK")
dev:write({})
print("[i2c_test] dev:write empty table OK")

dev:close()
print("[i2c_test] dev:close OK")
dev:close()
print("[i2c_test] dev:close idempotent OK")
expect_error("closed device access", function()
    dev:address()
end)

bus:close()
print("[i2c_test] bus:close OK")
bus:close()
print("[i2c_test] bus:close idempotent OK")
expect_error("closed bus access", function()
    bus:scan()
end)

print("[i2c_test] done")
