local mqtt = require("mqtt")

local uri = args.uri or "mqtts://broker.emqx.io:8883"
local topic = assert(args.topic, "args.topic is required")
local payload_size = args.payload_size or 4096
assert(type(payload_size) == "number" and payload_size >= 1024 and payload_size <= 32768, "payload_size must be in 1024..32768")

local payload = string.rep("mqtt-server-", math.ceil(payload_size / 12)):sub(1, payload_size)
local middle = math.floor(#payload / 2)
payload = payload:sub(1, middle) .. "\0" .. payload:sub(middle + 2)

local subscribed_id
local published_id
local unsubscribed_id
local received = false
local published = false
local unsubscribed = false
local failure

local function check(value, message)
    if value then return true end
    failure = message
    return false
end

local client = assert(mqtt.client({
    uri = uri,
    client_id = args.client_id,
    limits = { mqtt_buffer_size = 512, max_message_size = payload_size + 1024 },
}))

client:on("connected", function()
    local err
    subscribed_id, err = client:subscribe(topic, { qos = 1 })
    if not subscribed_id then failure = err end
end)

client:on("subscribed", function(event)
    if not check(event.packet_id == subscribed_id, "unexpected SUBACK packet id") then return end
    if not check(event.granted_qos == 1, "broker rejected requested QoS 1") then return end
    local err
    published_id, err = client:publish(topic, payload, { qos = 1 })
    if not published_id then failure = err end
end)

client:on("published", function(event)
    if event.packet_id == published_id then published = true end
end)

client:on("message", function(event)
    if not check(event.topic == topic, "unexpected topic") then return end
    if not check(event.qos == 1, "unexpected delivery QoS") then return end
    if not check(event.payload == payload, "payload mismatch") then return end
    received = true
    local err
    unsubscribed_id, err = client:unsubscribe(topic)
    if not unsubscribed_id then failure = err end
end)

client:on("unsubscribed", function(event)
    if not check(event.packet_id == unsubscribed_id, "unexpected UNSUBACK packet id") then return end
    unsubscribed = true
end)

client:on("error", function(event)
    failure = string.format("MQTT error type=%d esp_err=%d tls_error=%d errno=%d", event.error_type, event.esp_err, event.tls_error, event.socket_errno)
end)

assert(client:start())
local deadline = 300
while not failure and not (published and received and unsubscribed) and deadline > 0 do
    client:process_events(100, 8)
    deadline = deadline - 1
end

assert(not failure, failure)
assert(published, "PUBACK timeout")
assert(received, "server message timeout")
assert(unsubscribed, "UNSUBACK timeout")
local stats = client:stats()
assert(stats.events_dropped == 0 and stats.messages_dropped == 0 and stats.callback_errors == 0, "unexpected MQTT drops")
print("mqtt_server ok", uri, topic, #payload)
client:close()
