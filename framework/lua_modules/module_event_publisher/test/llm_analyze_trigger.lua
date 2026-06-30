local event_publisher = require("event_publisher")

local a = type(args) == "table" and args or {}

local channel = a.channel
local chat_id = a.chat_id
local analyze_text = a.analyze_text or a.text or "Please analyze this text with llm: "

local function require_string(name, value)
    if type(value) ~= "string" or value == "" then
        error("missing args." .. name)
    end
end

require_string("channel", channel)
require_string("chat_id", chat_id)

print(string.format(
    "[event_router_demo] publish trigger event_key=%s channel=%s chat_id=%s",
    "lua_llm_analyze",
    channel,
    chat_id
))

event_publisher.publish({
    source_cap = "lua_script",
    event_type = "trigger",
    source_channel = channel,
    target_channel = channel,
    chat_id = chat_id,
    content_type = "trigger",
    message_id = "lua_llm_analyze",
    correlation_id = "lua_llm_analyze",
    text = analyze_text,
    session_policy = "trigger",
    payload = {
        channel = channel,
        chat_id = chat_id,
        text = analyze_text,
    },
})

print("[event_router_demo] llm analyze trigger published")
