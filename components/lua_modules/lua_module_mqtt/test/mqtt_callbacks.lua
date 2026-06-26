-- MQTT callback demo (on/off/dispatch).
-- Mirrors knob/test/knob_events.lua: open resource in run(), register callbacks
-- with on(), then a service loop of dispatch() + delay. xpcall(run) + cleanup()
-- + rethrow. Defaults connect to the public test.mosquitto.org broker.
-- Optional args: uri, topic, duration_ms, poll_ms (see int/str helpers below).
local mqtt  = require("mqtt")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local function str_arg(k, default)
    local v = a[k]
    if type(v) == "string" then
        return v
    end
    return default
end
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local cfg_uri     = str_arg("uri", "mqtt://test.mosquitto.org:1883")
local cfg_topic   = str_arg("topic", "claw/announce")
local duration_ms = int_arg("duration_ms", 30000)
local poll_ms     = int_arg("poll_ms", 100)

local client

local function cleanup()
    if client then
        pcall(client.off, client)   -- remove all callbacks
        pcall(client.close, client)
        client = nil
    end
end

local function run()
    local c = mqtt.new(cfg_uri)
    client = c

    if not c:connect(10000) then
        error("[mqtt_cb] connect failed to " .. cfg_uri)
    end
    print("[mqtt_cb] connected to " .. cfg_uri)

    c:subscribe(cfg_topic, 0)

    local hits = 0
    c:on(cfg_topic, function(msg)
        hits = hits + 1
        print("[mqtt_cb] announce topic=" .. msg.topic .. " payload=" .. msg.payload)
    end)

    -- Publish to ourselves so the callback has something to fire on even with
    -- no external publisher (the broker echoes our own subscribed topic).
    c:publish(cfg_topic, "hello from esp-claw", 0, false)

    print(string.format(
        "[mqtt_cb] listening on '%s' for %d ms (dispatch every %d ms)...",
        cfg_topic, duration_ms, poll_ms))

    local iters = math.max(1, math.floor(duration_ms / poll_ms))
    for _ = 1, iters do
        c:dispatch()
        delay.delay_ms(poll_ms)
    end

    print("[mqtt_cb] done, callback fired " .. tostring(hits) .. " time(s)")
    if hits == 0 then
        error("[mqtt_cb] callback never fired")
    end
end

local run_ok, run_err = xpcall(run, debug.traceback)
cleanup()
print("[mqtt_cb] cleaned up")
if not run_ok then
    error(run_err)
end
