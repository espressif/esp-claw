local system = require("system")

local function print_kv(key, value)
    print(string.format("[system_demo] %s: %s", tostring(key), tostring(value)))
end

print("[system_demo] start")

print_kv("time", system.time())
print_kv("date", system.date("%Y-%m-%d %H:%M:%S"))
print_kv("millis", system.millis())
print_kv("uptime", system.uptime())

local ip = system.ip()
if ip then
    print_kv("ip", ip)
else
    print("[system_demo] ip: nil (Wi-Fi not connected or DHCP not ready)")
end

local info = system.info()
print_kv("sram_free", info.sram_free)
print_kv("sram_total", info.sram_total)
print_kv("sram_largest", info.sram_largest)

if info.psram_total ~= nil then
    print_kv("psram_free", info.psram_free)
    print_kv("psram_total", info.psram_total)
end

if info.wifi_ssid ~= nil then
    print_kv("wifi_ssid", info.wifi_ssid)
end

if info.wifi_rssi ~= nil then
    print_kv("wifi_rssi", info.wifi_rssi)
end

print("[system_demo] done")
