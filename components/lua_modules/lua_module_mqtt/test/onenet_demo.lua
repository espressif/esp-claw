local mqtt = require("mqtt")
local json = require("json")

local ONENET_CA = [[-----BEGIN CERTIFICATE-----
MIIDOzCCAiOgAwIBAgIJAPCCNfxANtVEMA0GCSqGSIb3DQEBCwUAMDQxCzAJBgNV
BAYTAkNOMQ4wDAYDVQQKDAVDTUlPVDEVMBMGA1UEAwwMT25lTkVUIE1RVFRTMB4X
DTE5MDUyOTAxMDkyOFoXDTQ5MDUyMTAxMDkyOFowNDELMAkGA1UEBhMCQ04xDjAM
BgNVBAoMBUNNSU9UMRUwEwYDVQQDDAxPbmVORVQgTVFUVFMwggEiMA0GCSqGSIb3
DQEBAQUAA4IBDwAwggEKAoIBAQC/VvJ6lGWfy9PKdXKBdzY83OERB35AJhu+9jkx
5d4SOtZScTe93Xw9TSVRKrFwu5muGgPusyAlbQnFlZoTJBZY/745MG6aeli6plpR
r93G6qVN5VLoXAkvqKslLZlj6wXy70/e0GC0oMFzqSP0AY74icANk8dUFB2Q8usS
UseRafNBcYfqACzF/Wa+Fu/upBGwtl7wDLYZdCm3KNjZZZstvVB5DWGnqNX9HkTl
U9NBMS/7yph3XYU3mJqUZxryb8pHLVHazarNRppx1aoNroi+5/t3Fx/gEa6a5PoP
ouH35DbykmzvVE67GUGpAfZZtEFE1e0E/6IB84PE00llvy3pAgMBAAGjUDBOMB0G
A1UdDgQWBBTTi/q1F2iabqlS7yEoX1rbOsz5GDAfBgNVHSMEGDAWgBTTi/q1F2ia
bqlS7yEoX1rbOsz5GDAMBgNVHRMEBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQAL
aqJ2FgcKLBBHJ8VeNSuGV2cxVYH1JIaHnzL6SlE5q7MYVg+Ofbs2PRlTiWGMazC7
q5RKVj9zj0z/8i3ScWrWXFmyp85ZHfuo/DeK6HcbEXJEOfPDvyMPuhVBTzuBIRJb
41M27NdIVCdxP6562n6Vp0gbE8kN10q+ksw8YBoLFP0D1da7D5WnSV+nwEIP+F4a
3ZX80bNt6tRj9XY0gM68mI60WXrF/qYL+NUz+D3Lw9bgDSXxpSN8JGYBR85BxBvR
NNAhsJJ3yoAvbPUQ4m8J/CoVKKgcWymS1pvEHmF47pgzbbjm5bdthlIx+swdiGFa
WzdhzTYwVkxBaU+xf/2w
-----END CERTIFICATE-----]]

local function required(name)
    local value = args[name]
    assert(type(value) == "string" and value ~= "", "args." .. name .. " is required")
    return value
end

local product_id = required("product_id")
local device_name = required("device_name")
local token = required("token")
local uri = args.uri or ("mqtts://" .. product_id .. ".mqttstls.acc.cmcconenet.cn:8883")
local duration_s = args.duration_s or 15
assert(type(duration_s) == "number" and duration_s == math.floor(duration_s) and duration_s >= 3 and duration_s <= 3600, "duration_s must be an integer in 3..3600")

-- OneNET spells the custom protocol namespace as "custome".
local base = "$sys/" .. product_id .. "/" .. device_name .. "/custome/"
local topics = {
    upload = base .. "up",
    upload_reply = base .. "up_reply",
    command = base .. "down/+",
    command_reply = base .. "down_reply/",
}

local payload = args.payload or json.encode({ temperature = 24.0, humidity = 55.0, status = "online" })
local failure, ready, upload_packet_id, upload_acked = nil, false, nil, false
local pending_subscriptions = {}

local function fail(message)
    if not failure then failure = message end
end

local client = assert(mqtt.client({
    uri = uri,
    client_id = device_name,
    username = product_id,
    password = token,
    tls = { ca = ONENET_CA, use_crt_bundle = false, skip_common_name_check = true },
    session = { clean_start = true, keep_alive = 60 },
    reconnect = { enabled = true },
    limits = { mqtt_buffer_size = 1024, max_message_size = 16384 },
}))

client:on("connected", function()
    pending_subscriptions = {}
    for _, topic in ipairs({ topics.upload_reply, topics.command }) do
        local packet_id, err = client:subscribe(topic, { qos = 1 })
        if not packet_id then fail(err or "subscribe failed") return end
        pending_subscriptions[packet_id] = true
    end
    print("onenet connected", uri)
end)

client:on("subscribed", function(event)
    if event.granted_qos == 0x80 then fail("OneNET rejected a subscription") return end
    pending_subscriptions[event.packet_id] = nil
    if next(pending_subscriptions) == nil and not ready then
        ready = true
        local err
        upload_packet_id, err = client:publish(topics.upload, payload, { qos = 1 })
        if not upload_packet_id then fail(err or "upload failed") return end
        print("onenet upload", #payload)
    end
end)

client:on("published", function(event)
    if event.packet_id == upload_packet_id then
        upload_acked = true
        print("onenet upload puback", event.packet_id)
    end
end)

client:on("message", function(event)
    if event.topic == topics.upload_reply then
        print("onenet upload reply", event.payload)
        return
    end
    local command_id = event.topic:match("/custome/down/([^/]+)$")
    if not command_id then return end
    print("onenet command", command_id, event.payload)
    local response = args.command_reply or json.encode({ code = 200, msg = "success" })
    local packet_id, err = client:publish(topics.command_reply .. command_id, response, { qos = 1 })
    if not packet_id then fail(err or "command reply failed") end
end)

client:on("error", function(event)
    fail(string.format("MQTT error type=%s esp_err=%s tls_error=%s errno=%s", event.error_type, event.esp_err, event.tls_error, event.socket_errno))
end)

assert(client:start())
local polls_remaining = duration_s * 5
while not failure and polls_remaining > 0 do
    client:process_events(200, 16)
    polls_remaining = polls_remaining - 1
end
local stats = client:stats()
client:close()
assert(not failure, failure)
assert(upload_acked, "upload PUBACK timeout")
assert(stats.events_dropped == 0 and stats.messages_dropped == 0 and stats.callback_errors == 0, "unexpected MQTT drops")
print("onenet demo ok")
