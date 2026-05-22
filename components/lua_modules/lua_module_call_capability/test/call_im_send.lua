-- IM send capability demo.
-- Supported args:
--   channel:     qq | telegram | tg | feishu | wechat | web | local, defaults to "qq"
--   chat_id:     target chat id
--   text:        optional text message
--   message:     optional text message, used when text is missing
--   image_path:  optional local image path to send
--   file_path:   optional local file path to send
--   caption:     optional caption for image/file sends
--   session_id:  optional call context session id
--   source_cap:  optional call context source cap, defaults to "lua_im_send_demo"
local capability = require("capability")

local a = type(args) == "table" and args or {}

local CHANNELS = {
    qq = {
        context_channel = "qq",
        text = "qq_send_message",
        image = "qq_send_image",
        file = "qq_send_file",
    },
    telegram = {
        context_channel = "telegram",
        text = "tg_send_message",
        image = "tg_send_image",
        file = "tg_send_file",
    },
    tg = {
        context_channel = "telegram",
        text = "tg_send_message",
        image = "tg_send_image",
        file = "tg_send_file",
    },
    feishu = {
        context_channel = "feishu",
        text = "feishu_send_message",
        image = "feishu_send_image",
        file = "feishu_send_file",
    },
    wechat = {
        context_channel = "wechat",
        text = "wechat_send_message",
        image = "wechat_send_image",
    },
    web = {
        context_channel = "web",
        text = "local_send_message",
    },
    local = {
        context_channel = "web",
        text = "local_send_message",
    },
}

local function string_arg(key, default)
    local value = a[key]
    if type(value) == "string" and value ~= "" then
        return value
    end
    return default
end

local function normalize_channel(channel)
    return string.lower(channel)
end

local function build_opts(config)
    local opts = {
        channel = config.context_channel,
        source_cap = string_arg("source_cap", "lua_im_send_demo"),
    }

    local session_id = string_arg("session_id", nil)
    local chat_id = string_arg("chat_id", nil)

    if session_id then
        opts.session_id = session_id
    end
    if chat_id then
        opts.chat_id = chat_id
    end

    return opts
end

local function call_required(cap_name, payload, opts, label)
    local ok, out, err

    print(string.format("[call_im_send] calling %s: %s", label, cap_name))
    ok, out, err = capability.call(cap_name, payload, opts)
    print(string.format(
        "[call_im_send] result label=%s cap=%s ok=%s out=%s err=%s",
        tostring(label), tostring(cap_name), tostring(ok), tostring(out), tostring(err)))

    if not ok then
        error(string.format("%s failed: %s", cap_name, tostring(err or out)))
    end
end

local function text_payload(chat_id, message)
    return {
        chat_id = chat_id,
        message = message,
    }
end

local function media_payload(chat_id, path, caption)
    local payload = {
        chat_id = chat_id,
        path = path,
    }

    if caption then
        payload.caption = caption
    end

    return payload
end

local function run()
    local channel = normalize_channel(string_arg("channel", "qq"))
    local config = CHANNELS[channel]
    local opts
    local chat_id
    local message
    local image_path
    local file_path
    local caption

    if not config then
        error(string.format("[call_im_send] unsupported channel: %s", tostring(channel)))
    end

    opts = build_opts(config)
    chat_id = string_arg("chat_id", nil)
    message = string_arg("text", string_arg("message", "hello from lua capability im send"))
    image_path = string_arg("image_path", nil)
    file_path = string_arg("file_path", nil)
    caption = string_arg("caption", nil)

    if not chat_id then
        error("[call_im_send] args.chat_id is required")
    end

    call_required(config.text, text_payload(chat_id, message), opts, "text")

    if image_path then
        if not config.image then
            error(string.format("[call_im_send] channel %s does not support image send", channel))
        end
        call_required(config.image, media_payload(chat_id, image_path, caption), opts, "image")
    end

    if file_path then
        if not config.file then
            error(string.format("[call_im_send] channel %s does not support file send", channel))
        end
        call_required(config.file, media_payload(chat_id, file_path, caption), opts, "file")
    end

    print("[call_im_send] done")
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    error(err)
end
