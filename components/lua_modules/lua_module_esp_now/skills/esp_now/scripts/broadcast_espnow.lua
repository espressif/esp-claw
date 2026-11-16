-- ESP-NOW one-off broadcast entry point for Agent calls.
--
-- Initializes ESP-NOW, adds the broadcast peer, sends a single broadcast
-- message, waits for the send-complete event, then tears down.
--
-- Usage:
--   lua --run --path builtin/skills/esp_now/scripts/broadcast_espnow.lua

local CONFIG = {
    message       = "hello from esp-claw",
    channel       = 0,   -- 0 = use current WiFi channel
    event_poll_ms = 100,
    wait_cycles   = 20,
}

local esp_now = require("esp_now")

local function log(msg)
    print("[espnow_broadcast] " .. msg)
end

local function assert_ok(ok, err)
    if not ok then
        error(tostring(err))
    end
    return ok
end

local done = false

assert_ok(esp_now.init())

assert_ok(esp_now.on_event(function(ev)
    if ev.type == "send" then
        log("send complete success=" .. tostring(ev.success))
        done = true
    end
end))

assert_ok(esp_now.add_peer({
    peer_addr = esp_now.BROADCAST_MAC,
    channel = CONFIG.channel,
    ifidx = esp_now.IFIDX.STA,
}))

log("broadcasting: " .. CONFIG.message)
assert_ok(esp_now.send(esp_now.BROADCAST_MAC, CONFIG.message))

for _ = 1, CONFIG.wait_cycles do
    esp_now.process_events(CONFIG.event_poll_ms)
    if done then
        break
    end
end

esp_now.deinit()
log("done")
