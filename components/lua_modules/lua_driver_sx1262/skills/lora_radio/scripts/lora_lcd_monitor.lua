local sx1262 = require("sx1262")
local board_manager = require("board_manager")
local display = require("display")
local system = require("system")
local delay = require("delay")

local a = type(args) == "table" and args or {}

local function integer_arg(value, name, min_value, max_value)
    local integer = type(value) == "number" and math.tointeger(value) or nil
    assert(integer ~= nil, name .. " must be an integer")
    assert(integer >= min_value, name .. " must be >= " .. min_value)
    if max_value ~= nil then
        assert(integer <= max_value, name .. " must be <= " .. max_value)
    end
    return integer
end

local frequency_hz = integer_arg(assert(a.frequency_hz, "frequency_hz is required"),
    "frequency_hz", 150000000, 960000000)
local bandwidth_khz = integer_arg(a.bandwidth_khz or 125, "bandwidth_khz", 7, 500)
local spreading_factor = integer_arg(a.spreading_factor or 9, "spreading_factor", 5, 12)
local coding_rate = integer_arg(a.coding_rate or 6, "coding_rate", 5, 8)
local tx_power_dbm = integer_arg(a.tx_power_dbm or 22, "tx_power_dbm", -9, 22)
local current_limit_ma = integer_arg(a.current_limit_ma or 140, "current_limit_ma", 60, 140)
local preamble_length = integer_arg(a.preamble_length or 16, "preamble_length", 1, 65535)
local sync_word = integer_arg(a.sync_word or 0xAB, "sync_word", 0, 255)
local tx_timeout_ms = integer_arg(a.tx_timeout_ms or 5000, "tx_timeout_ms", 1, 262143)
local interval_ms = a.interval_ms and integer_arg(a.interval_ms, "interval_ms", 100) or nil

assert(a.send_text == nil or type(a.send_text) == "string", "send_text must be a string")
assert(a.send_hex == nil or type(a.send_hex) == "string", "send_hex must be a string")
assert(not (a.send_text ~= nil and a.send_hex ~= nil),
    "send_text and send_hex cannot be used together")

local function parse_hex(value)
    local compact = value:gsub("%s+", "")
    assert(#compact % 2 == 0, "send_hex must contain an even number of hex digits")
    assert(compact:match("^[0-9A-Fa-f]*$"), "send_hex contains a non-hex character")
    assert(#compact <= 510, "send_hex must contain at most 255 bytes")
    local bytes = {}
    for i = 1, #compact, 2 do
        bytes[#bytes + 1] = tonumber(compact:sub(i, i + 1), 16)
    end
    return bytes
end

local tx_payload = a.send_text
if a.send_hex ~= nil then
    tx_payload = parse_hex(a.send_hex)
end
if type(tx_payload) == "string" then
    assert(#tx_payload <= 255, "send_text must contain at most 255 bytes")
end

local radio
local display_initialized = false

local function bytes_hex(data)
    local parts = {}
    for i = 1, #data do
        parts[i] = string.format("%02X", data:byte(i))
    end
    return table.concat(parts)
end

local function table_hex(data)
    local parts = {}
    for i = 1, #data do
        parts[i] = string.format("%02X", data[i])
    end
    return table.concat(parts)
end

local function payload_label(payload)
    local text
    if type(payload) == "table" then
        text = table_hex(payload)
    elseif payload:match("^[%g ]*$") then
        text = payload
    else
        text = bytes_hex(payload)
    end
    if #text > 30 then
        return text:sub(1, 27) .. "..."
    end
    return text
end

local function render(tx_line, rx_line, signal_line)
    local line_height = 32
    local total_height = line_height * 4
    local top = math.max(0, (display.height - total_height) // 2)
    local mhz = frequency_hz / 1000000

    display.begin_frame({ clear = true, color = "#101214", preserve = false })
    display.draw_text_aligned(0, top, display.width, line_height,
        string.format("LoRa %.3fMHz BW%d SF%d", mhz, bandwidth_khz, spreading_factor), {
            color = "#F4D03F", font_size = 16, align = "center", valign = "middle",
        })
    display.draw_text_aligned(0, top + line_height, display.width, line_height, tx_line, {
        color = "#58D68D", font_size = 18, align = "center", valign = "middle",
    })
    display.draw_text_aligned(0, top + line_height * 2, display.width, line_height, rx_line, {
        color = "#5DADE2", font_size = 18, align = "center", valign = "middle",
    })
    display.draw_text_aligned(0, top + line_height * 3, display.width, line_height, signal_line, {
        color = "#D5D8DC", font_size = 16, align = "center", valign = "middle",
    })
    display.present()
    display.end_frame()
end

local function cleanup()
    if radio then
        pcall(function() radio:close() end)
        radio = nil
    end
    if display_initialized then
        pcall(display.end_frame)
        pcall(display.deinit)
        display_initialized = false
    end
end

local function compact_error(err)
    local text = tostring(err):gsub("\r", " "):gsub("\n.*", "")
    text = text:match("sx1262%s+(.+)") or text
    if #text > 38 then
        text = text:sub(1, 35) .. "..."
    end
    return text
end

local function hold_error(err)
    local detail = compact_error(err)
    print("[lora_lcd_monitor] ERROR: " .. tostring(err))
    pcall(render, "INIT ERROR", detail, "CHECK MODULE / SERIAL LOG")

    -- Keep the diagnostic visible until this named async job is replaced.
    while true do
        delay.delay_ms(1000)
    end
end

local function open_display()
    local panel_handle, io_handle, width, height, panel_if, pixel_format =
        board_manager.get_display_lcd_params("display_lcd")
    assert(panel_handle, "display_lcd is unavailable: " .. tostring(io_handle))
    display.init(panel_handle, io_handle, width, height, panel_if, pixel_format)
    display_initialized = true
    display.backlight(true)
end

local function open_radio()
    local ok, result = pcall(sx1262.new, {
        spi_host = 2,
        cs_gpio = 14,
        reset_gpio = 42,
        dio1_gpio = 45,
        busy_gpio = 38,
        frequency_hz = frequency_hz,
        bandwidth_khz = bandwidth_khz,
        spreading_factor = spreading_factor,
        coding_rate = coding_rate,
        tx_power_dbm = tx_power_dbm,
        current_limit_ma = current_limit_ma,
        preamble_length = preamble_length,
        sync_word = sync_word,
        -- Matches HPD16A_TCXO and RadioLib's radio.begin() default.
        tcxo_voltage = 1.6,
        tcxo_delay_us = 5000,
        use_dcdc = true,
        dio2_rf_switch = true,
        crc = false,
        invert_iq = false,
    })
    if not ok then
        hold_error(result)
    end
    radio = result
end

local function run()
    open_display()
    render("STATUS: INITIALIZING", "RX: --", "SX1262 HPD16A  SPI3")
    open_radio()

    local tx_line = "TX: --"
    local rx_line = "RX: --"
    local signal_line = "RSSI: --  SNR: --"
    local next_tx_ms = tx_payload ~= nil and system.millis() or nil
    render(tx_line, rx_line, signal_line)

    local receive_started, receive_start_err = radio:start_receive()
    if not receive_started then
        hold_error("start receive failed: " .. tostring(receive_start_err))
    end
    while true do
        local now = system.millis()
        if next_tx_ms ~= nil and now >= next_tx_ms then
            local ok, send_err = radio:send(tx_payload, tx_timeout_ms)
            if ok then
                tx_line = "TX: " .. payload_label(tx_payload)
            else
                tx_line = "TX ERROR: " .. tostring(send_err)
                print("[lora_lcd_monitor] TX failed: " .. tostring(send_err))
                local status_ok, status = pcall(function() return radio:status() end)
                if status_ok then
                    signal_line = string.format("MODE:%s ERR:%d",
                        tostring(status.mode), status.device_errors)
                    print(string.format(
                        "[lora_lcd_monitor] radio mode=%s command=%d device_errors=%d",
                        tostring(status.mode), status.command_status, status.device_errors))
                end
            end
            local resumed, resume_err = radio:start_receive()
            if not resumed then
                hold_error("resume receive failed: " .. tostring(resume_err))
            end
            next_tx_ms = interval_ms and (system.millis() + interval_ms) or nil
            render(tx_line, rx_line, signal_line)
        end

        local packet, receive_err = radio:receive(100)
        if packet then
            rx_line = "RX: " .. payload_label(packet.data)
            signal_line = string.format("RSSI: %.1f dBm  SNR: %.1f dB",
                packet.rssi, packet.snr)
            render(tx_line, rx_line, signal_line)
            print(string.format("[lora_lcd_monitor] RX %d bytes RSSI %.1f SNR %.1f",
                packet.length, packet.rssi, packet.snr))
        elseif receive_err ~= "timeout" then
            rx_line = "RX ERROR: " .. tostring(receive_err)
            render(tx_line, rx_line, signal_line)
            print("[lora_lcd_monitor] RX failed: " .. tostring(receive_err))
            if receive_err == "radio_timeout" then
                local resumed, resume_err = radio:start_receive()
                if not resumed then
                    hold_error("recover receive failed: " .. tostring(resume_err))
                end
            end
        end
    end
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    cleanup()
    print("[lora_lcd_monitor] ERROR: " .. tostring(err))
    error(err)
end
cleanup()
