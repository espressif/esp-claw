-- ESP-NOW bring-up and self-test.
--
-- Verifies init, version, peer management, broadcast send, event processing,
-- and deinit. With two boards running this script, each device broadcasts a
-- greeting and prints any frames it receives from the other.

local LOG_PREFIX = "[espnow_test]"
local MESSAGE = "ping from esp-claw"
local POLL_MS = 100
local LOOP_CYCLES = 50

local delay_ok, delay = pcall(require, "delay")
local esp_now = require("esp_now")

local function log(msg)
    print(LOG_PREFIX .. " " .. msg)
end

local function assert_ok(ok, err)
    if not ok then
        error(LOG_PREFIX .. " " .. tostring(err))
    end
    return ok
end

local function sleep_ms(ms)
    if delay_ok and delay and type(delay.delay_ms) == "function" then
        delay.delay_ms(ms)
        return
    end
    if os and type(os.clock) == "function" then
        local deadline = os.clock() + (ms / 1000)
        while os.clock() < deadline do end
    end
end

local function mac_to_hex(mac)
    if type(mac) ~= "string" or #mac ~= 6 then
        return "??"
    end
    return string.format("%02x:%02x:%02x:%02x:%02x:%02x",
        mac:byte(1), mac:byte(2), mac:byte(3),
        mac:byte(4), mac:byte(5), mac:byte(6))
end

local recv_count = 0
local send_count = 0

-- init
assert_ok(esp_now.init())
log("init ok; version=" .. tostring(esp_now.get_version()))

-- radio helpers
local local_mac = assert_ok(esp_now.get_mac())
assert(#local_mac == 6, "get_mac must return a 6-byte string")
log("local mac=" .. mac_to_hex(local_mac))
local channel = assert_ok(esp_now.get_channel())
log("current channel=" .. tostring(channel))

-- events
assert_ok(esp_now.on_event(function(ev)
    if ev.type == "recv" then
        recv_count = recv_count + 1
        log("recv from=" .. mac_to_hex(ev.src_mac)
            .. " rssi=" .. tostring(ev.rssi)
            .. " data=" .. tostring(ev.data))
    elseif ev.type == "send" then
        send_count = send_count + 1
        log("send success=" .. tostring(ev.success))
    end
end))

-- peer management
assert_ok(esp_now.add_peer({ peer_addr = esp_now.BROADCAST_MAC, channel = 0 }))
assert(esp_now.peer_exists(esp_now.BROADCAST_MAC), "broadcast peer should exist")

local num = assert_ok(esp_now.get_peer_num())
log("peer_num total=" .. tostring(num.total) .. " encrypt=" .. tostring(num.encrypt))

local peers = esp_now.fetch_peers()
log("fetch_peers count=" .. tostring(#peers))

local peer = assert_ok(esp_now.get_peer(esp_now.BROADCAST_MAC))
log("broadcast peer channel=" .. tostring(peer.channel) .. " ifidx=" .. tostring(peer.ifidx))

-- send
assert_ok(esp_now.send(esp_now.BROADCAST_MAC, MESSAGE))

-- event loop
for _ = 1, LOOP_CYCLES do
    esp_now.process_events(POLL_MS)
    sleep_ms(10)
end

log("totals: recv=" .. tostring(recv_count) .. " send=" .. tostring(send_count))

-- cleanup
assert_ok(esp_now.del_peer(esp_now.BROADCAST_MAC))
assert_ok(esp_now.deinit())
log("deinit ok; test complete")
