-- ──────────────────────────────────────────────────────────────
-- Connect to an MQTT broker, publish a status payload, subscribe to a
-- command topic, then poll for one inbound message.
-- ──────────────────────────────────────────────────────────────

-- 1. Requires
local mqtt = require("mqtt")

-- 2. Constants
local BROKER_URI = "mqtt://127.0.0.1:1883"
local STATUS_TOPIC = "casa/esp32/estado"
local CMD_TOPIC = "casa/esp32/cmd"
local CONNECT_TIMEOUT_MS = 10000
local POLL_TIMEOUT_MS = 5000
local QOS = 1
local RETAIN = false

-- 3. Module-local state
local client = nil

-- 4. Cleanup
local function cleanup()
  if client then
    pcall(client.disconnect, client)
    pcall(client.close, client)
    client = nil
  end
end

-- 5. Run
local function run()
  client = mqtt.new(BROKER_URI, { client_id = "esp_claw_test" })

  if not client:connect(CONNECT_TIMEOUT_MS) then
    error("mqtt: broker connect timed out")
  end
  print("[mqtt_pubsub] connected")

  local msg_id = client:publish(STATUS_TOPIC, '{"led":"rojo"}', QOS, RETAIN)
  print("[mqtt_pubsub] published id=" .. tostring(msg_id))

  client:subscribe(CMD_TOPIC, QOS)
  print("[mqtt_pubsub] subscribed " .. CMD_TOPIC)

  local msg = client:poll(POLL_TIMEOUT_MS)
  if msg then
    print("[mqtt_pubsub] rx topic=" .. msg.topic .. " payload=" .. msg.payload)
  else
    print("[mqtt_pubsub] no inbound message within timeout")
  end
end

-- 6. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then error(err) end
