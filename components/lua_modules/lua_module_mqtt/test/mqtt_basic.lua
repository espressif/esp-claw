local mqtt = require("mqtt")

assert(type(args.uri) == "string" and args.uri ~= "", "args.uri is required")
local topic = args.topic or "claw/test"
local payload = args.payload or "hello from esp-claw"
local received = false

local client = assert(mqtt.client({
    uri = args.uri,
    client_id = args.client_id,
    username = args.username,
    password = args.password,
    limits = { mqtt_buffer_size = 1024, max_message_size = 16384 },
}))

client:on("connected", function()
    assert(client:subscribe(topic, { qos = 1 }))
end)

client:on("subscribed", function()
    assert(client:publish(topic, payload, { qos = 1 }))
end)

client:on("message", function(event)
    assert(event.topic == topic)
    assert(event.payload == payload)
    received = true
    print("mqtt_basic received", event.topic, #event.payload)
end)

client:on("error", function(event)
    print("mqtt_basic error", event.error_type, event.esp_err, event.socket_errno)
end)

assert(client:start())
local deadline = 150
while not received and deadline > 0 do
    client:process_events(100, 8)
    deadline = deadline - 1
end
assert(received, "MQTT loopback timeout")
print("mqtt_basic ok")
client:close()
