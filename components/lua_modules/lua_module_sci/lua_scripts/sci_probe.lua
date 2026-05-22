local sci = require("sci")

local candidates = {
    { name = "K10 onboard I2C", port = 1, sda = 47, scl = 48 },
    { name = "generic Grove I2C", port = 0, sda = 21, scl = 22 },
    { name = "default external I2C", port = 0, sda = 14, scl = 13 },
}

local addresses = { 0x21, 0x22, 0x23 }

local function try_one(bus, addr)
    local dev, err = sci.new(bus.port, bus.sda, bus.scl, addr, 100000)
    if not dev then
        return false, "open failed: " .. tostring(err)
    end

    dev:set_timeout(1200)
    local ok, version = pcall(function()
        return dev:get_version()
    end)

    if not ok then
        dev:close()
        return false, tostring(version)
    end

    local info_ok, info = pcall(function()
        return dev:get_information(sci.ALL, true)
    end)
    local sku_ok, sku = pcall(function()
        return dev:get_sku(sci.ALL)
    end)

    print(string.format("FOUND %s port=%d sda=%d scl=%d addr=0x%02X",
        bus.name, bus.port, bus.sda, bus.scl, addr))
    print("version: " .. tostring(version.text))
    print("sku: " .. (sku_ok and tostring(sku) or ("ERROR: " .. tostring(sku))))
    print("information: " .. (info_ok and tostring(info) or ("ERROR: " .. tostring(info))))

    dev:close()
    return true
end

print("DFRobot SCI probe start")
for _, bus in ipairs(candidates) do
    print(string.format("try %s port=%d sda=%d scl=%d",
        bus.name, bus.port, bus.sda, bus.scl))
    for _, addr in ipairs(addresses) do
        local ok, reason = try_one(bus, addr)
        if ok then
            return
        end
        print(string.format("miss addr=0x%02X: %s", addr, reason))
    end
end

error("SCI module not found on tested buses/addresses")
