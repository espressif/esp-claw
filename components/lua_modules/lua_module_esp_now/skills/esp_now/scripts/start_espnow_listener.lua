-- ESP-NOW listener entry point for Agent calls.
--
-- Initializes ESP-NOW, registers the broadcast peer so broadcast frames are
-- accepted, prints this device's WiFi MAC, and runs an event loop printing
-- received frames until the Lua runtime requests a stop.
--
-- Usage:
--   lua --run --path builtin/skills/esp_now/scripts/start_espnow_listener.lua

local CONFIG = {
    channel       = 0,   -- 0 = use current WiFi channel
    event_poll_ms = 100,
}

local esp_now = require("esp_now")

local function log(msg)
    print("[espnow_listener] " .. msg)
end

local function assert_ok(ok, err)
    if not ok then
        error(tostring(err))
    end
    return ok
end

local function mac_to_hex(mac)
    if type(mac) ~= "string" or #mac ~= 6 then
        return "??:??:??:??:??:??"
    end
    return string.format("%02x:%02x:%02x:%02x:%02x:%02x",
        mac:byte(1), mac:byte(2), mac:byte(3),
        mac:byte(4), mac:byte(5), mac:byte(6))
end

local function on_event(ev)
    if ev.type == "recv" then
        log("recv from=" .. mac_to_hex(ev.src_mac)
            .. " rssi=" .. tostring(ev.rssi)
            .. " ch=" .. tostring(ev.channel)
            .. " len=" .. tostring(#ev.data)
            .. " data=" .. tostring(ev.data))
    elseif ev.type == "send" then
        log("send peer=" .. mac_to_hex(ev.peer_mac)
            .. " success=" .. tostring(ev.success))
    end
end

log("initializing ESP-NOW")
assert_ok(esp_now.init())

-- Accept broadcast frames.
assert_ok(esp_now.add_peer({
    peer_addr = esp_now.BROADCAST_MAC,
    channel = CONFIG.channel,
    ifidx = esp_now.IFIDX.STA,
}))

assert_ok(esp_now.on_event(on_event))

local local_mac = esp_now.get_mac()
log("local mac=" .. mac_to_hex(local_mac) .. " (share this with the peer board)")
log("current channel=" .. tostring(esp_now.get_channel()))
log("ESP-NOW version=" .. tostring(esp_now.get_version()))
log("entering event loop (poll every " .. CONFIG.event_poll_ms .. " ms)")
while true do
    esp_now.process_events(CONFIG.event_poll_ms)
end
