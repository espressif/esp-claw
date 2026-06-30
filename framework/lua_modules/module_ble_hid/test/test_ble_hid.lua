-- Minimal BLE HID Consumer Control test.
-- Pair from the phone OS Bluetooth settings, then keep the script running.

local LOG_PREFIX = "[hid_test]"
local HID_NAME = "esp-claw-hid"
local WAIT_CONNECT_MS = 20000
local SUBSCRIBE_SETTLE_MS = 8000
local SEND_INTERVAL_MS = 3000
local AUTO_MEDIA_ROUNDS = 2
local KEYBOARD_KEYS = {
    "A",
    "ENTER",
    "ESC",
    "BACKSPACE",
    "TAB",
    "SPACE",
    "LEFT",
    "RIGHT",
    "PAGE_UP",
    "PAGE_DOWN",
}
local COMBOS = {
    { "CTRL", "C" },
    { "CTRL", "V" },
    { "CTRL", "ENTER" },
    { "ALT", "TAB" },
    { "COMMAND", "SPACE" },
}
local TEXT_SAMPLES = {
    "hello",
    "Hello 123",
    "test@example.com",
}
local MEDIA_KEYS = {
    { name = "play_pause", report = 0x04 },
    { name = "volume_up", report = 0x01 },
    { name = "volume_down", report = 0x02 },
    { name = "next_track", report = 0x08 },
    { name = "previous_track", report = 0x10 },
    { name = "mute", report = 0x20 },
}
local MOUSE_BUTTONS = {
    "left",
    "right",
    "middle",
}
local MOUSE_MOVES = {
    { 30, 0 },
    { 0, 30 },
    { -30, 0 },
    { 0, -30 },
}

local delay_ok, delay = pcall(require, "delay")
local hid_module = nil

local function sleep_ms(ms)
    if delay_ok and delay and type(delay.delay_ms) == "function" then
        delay.delay_ms(ms)
        return
    end

    if os and type(os.clock) == "function" then
        local deadline = os.clock() + (ms / 1000)
        while os.clock() < deadline do
        end
        return
    end

    for _ = 1, ms * 1000 do
    end
end

local function table_to_string(value)
    if type(value) ~= "table" then
        return tostring(value)
    end

    local parts = {}
    for key, item in pairs(value) do
        parts[#parts + 1] = tostring(key) .. "=" .. tostring(item)
    end
    return "{" .. table.concat(parts, ", ") .. "}"
end

local function log(message)
    print(LOG_PREFIX .. " " .. message)
end

local function print_status(ble_hid, label)
    local ok, status = pcall(ble_hid.status)
    log("status " .. label .. " -> ok=" .. tostring(ok) .. " result=" .. table_to_string(status))
    if not ok then
        error(status)
    end
    return status
end

local function call_step(ble_hid, step, fn)
    log("step: " .. step)
    local ok, result, err = pcall(fn)
    log(step .. " -> ok=" .. tostring(ok)
        .. " result=" .. table_to_string(result)
        .. " err=" .. tostring(err))
    print_status(ble_hid, "after " .. step)
    if not ok then
        error(result)
    end
    if result == nil then
        error(step .. " failed: " .. tostring(err))
    end
end

local function wait_for_connection(ble_hid)
    local elapsed = 0

    while elapsed < WAIT_CONNECT_MS do
        local status = print_status(ble_hid, "waiting")
        if status.connected then
            return true
        end
        sleep_ms(1000)
        elapsed = elapsed + 1000
    end

    return false
end

local function is_auto_media_enabled()
    if type(args) ~= "table" then
        return false
    end

    local value = args.auto_media
    return value == true or value == "true" or value == 1 or value == "1"
end

local function arg_enabled(name)
    if type(args) ~= "table" then
        return false
    end

    local value = args[name]
    return value == true or value == "true" or value == 1 or value == "1"
end

local function run_auto_media_test(ble_hid)
    log("auto_media enabled")
    for round = 1, AUTO_MEDIA_ROUNDS do
        log(string.format("auto media test round %d/%d", round, AUTO_MEDIA_ROUNDS))
        for _, media_key in ipairs(MEDIA_KEYS) do
            local step = string.format("ble_hid.media %s report=0x%02x", media_key.name, media_key.report)

            call_step(ble_hid, step, function()
                return ble_hid.media(media_key.name)
            end)
            sleep_ms(SEND_INTERVAL_MS)
        end
    end
    log("auto media test complete")
end

local function run_auto_keyboard_test(ble_hid)
    log("auto_keyboard enabled")
    for _, key in ipairs(KEYBOARD_KEYS) do
        call_step(ble_hid, "ble_hid.key " .. key, function()
            return ble_hid.key(key)
        end)
        sleep_ms(SEND_INTERVAL_MS)
    end
    log("auto keyboard test complete")
end

local function run_auto_combo_test(ble_hid)
    log("auto_combo enabled")
    for _, combo in ipairs(COMBOS) do
        call_step(ble_hid, "ble_hid.combo " .. table.concat(combo, "+"), function()
            local unpack_fn = table.unpack or unpack
            return ble_hid.combo(unpack_fn(combo))
        end)
        sleep_ms(SEND_INTERVAL_MS)
    end
    log("auto combo test complete")
end

local function run_auto_text_test(ble_hid)
    log("auto_text enabled")
    for _, sample in ipairs(TEXT_SAMPLES) do
        call_step(ble_hid, "ble_hid.text " .. sample, function()
            return ble_hid.text(sample)
        end)
        sleep_ms(SEND_INTERVAL_MS)
    end
    log("auto text test complete")
end

local function run_auto_mouse_test(ble_hid)
    log("auto_mouse enabled")
    for _, move in ipairs(MOUSE_MOVES) do
        call_step(ble_hid, "ble_hid.mouse_move " .. tostring(move[1]) .. "," .. tostring(move[2]), function()
            return ble_hid.mouse_move(move[1], move[2])
        end)
        sleep_ms(500)
    end
    for _, button_name in ipairs(MOUSE_BUTTONS) do
        call_step(ble_hid, "ble_hid.mouse_button " .. button_name, function()
            return ble_hid.mouse_button(button_name, "click")
        end)
        sleep_ms(SEND_INTERVAL_MS)
    end
    call_step(ble_hid, "ble_hid.mouse drag", function()
        local ok, err = ble_hid.mouse_button("left", "down")
        if not ok then
            return nil, err
        end
        sleep_ms(200)
        ok, err = ble_hid.mouse_move(40, 0)
        if not ok then
            return nil, err
        end
        sleep_ms(200)
        return ble_hid.mouse_button("left", "up")
    end)
    sleep_ms(SEND_INTERVAL_MS)
    call_step(ble_hid, "ble_hid.mouse_scroll vertical", function()
        return ble_hid.mouse_scroll(-3, 0)
    end)
    sleep_ms(SEND_INTERVAL_MS)
    call_step(ble_hid, "ble_hid.mouse_scroll horizontal", function()
        return ble_hid.mouse_scroll(0, 3)
    end)
    log("auto mouse test complete")
end

local function idle_for_manual_commands(ble_hid)
    log("auto_media disabled")
    log("waiting for manual ble_hid.media() commands")
    while true do
        print_status(ble_hid, "manual idle")
        sleep_ms(5000)
    end
end

local function run()
    log("require ble_hid")
    local ble_hid = require("ble_hid")
    hid_module = ble_hid
    log("require ble_hid -> " .. tostring(ble_hid))

    call_step(ble_hid, "ble_hid.init", function()
        return ble_hid.init()
    end)

    call_step(ble_hid, "ble_hid.start", function()
        return ble_hid.start({
            name = HID_NAME,
        })
    end)

    log("pair from system Bluetooth settings with device name " .. HID_NAME)
    log("现在可以进行手机蓝牙配对")
    log("20 秒配对窗口开始")
    if not wait_for_connection(ble_hid) then
        error("timeout waiting for HID connection")
    end
    log("connected; waiting " .. tostring(SUBSCRIBE_SETTLE_MS) .. " ms for HID report subscription")
    sleep_ms(SUBSCRIBE_SETTLE_MS)
    print_status(ble_hid, "ready")
    local ok, description = pcall(ble_hid.describe)
    log("describe -> ok=" .. tostring(ok) .. " result=" .. table_to_string(description))

    if is_auto_media_enabled() then
        run_auto_media_test(ble_hid)
    end
    if arg_enabled("auto_keyboard") then
        run_auto_keyboard_test(ble_hid)
    end
    if arg_enabled("auto_combo") then
        run_auto_combo_test(ble_hid)
    end
    if arg_enabled("auto_text") then
        run_auto_text_test(ble_hid)
    end
    if arg_enabled("auto_mouse") then
        run_auto_mouse_test(ble_hid)
    end
    if is_auto_media_enabled() or arg_enabled("auto_keyboard") or arg_enabled("auto_combo") or
        arg_enabled("auto_text") or arg_enabled("auto_mouse") then
        return
    end

    idle_for_manual_commands(ble_hid)
end

local function cleanup()
    if hid_module then
        pcall(hid_module.release_all)
        pcall(hid_module.stop)
        pcall(hid_module.deinit)
        hid_module = nil
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    log("ERROR: " .. tostring(err))
    error(err)
end
