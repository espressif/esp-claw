-- Scan the I2C bus, read 4 bytes from each detected device, write 4 test bytes,
-- then read back the same registers to verify the write.
local i2c = require("i2c")

local I2C_PORT = 0
local SDA_GPIO = 14
local SCL_GPIO = 13
local FREQ_HZ = 400000

local START_REG = 0x00
local TEST_DATA = {0x12, 0x34, 0x56, 0x78}

local function format_bytes(data)
    local parts = {}
    for i = 1, #data do
        parts[i] = string.format("0x%02X", string.byte(data, i))
    end
    return table.concat(parts, " ")
end

local bus = i2c.new(I2C_PORT, SDA_GPIO, SCL_GPIO, FREQ_HZ)

print(string.format(
    "[i2c_demo] scanning I2C port=%d sda=%d scl=%d freq=%d",
    I2C_PORT,
    SDA_GPIO,
    SCL_GPIO,
    FREQ_HZ
))

local addrs = bus:scan()
if #addrs == 0 then
    print("[i2c_demo] no I2C device found")
    bus:close()
    print("[i2c_demo] done")
    return
end

for _, addr in ipairs(addrs) do
    print(string.format("[i2c_demo] found device at 0x%02X", addr))
end

for _, addr in ipairs(addrs) do
    local dev = bus:device(addr)

    local before = dev:read(4, START_REG)
    print(string.format(
        "[i2c_demo] device 0x%02X read 4 bytes from 0x%02X: %s",
        addr,
        START_REG,
        format_bytes(before)
    ))

    dev:write(TEST_DATA, START_REG)
    print(string.format(
        "[i2c_demo] device 0x%02X wrote 4 bytes to 0x%02X: 0x%02X 0x%02X 0x%02X 0x%02X",
        addr,
        START_REG,
        TEST_DATA[1],
        TEST_DATA[2],
        TEST_DATA[3],
        TEST_DATA[4]
    ))

    local after = dev:read(4, START_REG)
    print(string.format(
        "[i2c_demo] device 0x%02X read back 4 bytes from 0x%02X: %s",
        addr,
        START_REG,
        format_bytes(after)
    ))

    if after == string.char(TEST_DATA[1], TEST_DATA[2], TEST_DATA[3], TEST_DATA[4]) then
        print(string.format("[i2c_demo] device 0x%02X write verification OK", addr))
    else
        print(string.format("[i2c_demo] device 0x%02X write verification FAILED", addr))
    end

    dev:close()
end
bus:close()
print("[i2c_demo] done")
