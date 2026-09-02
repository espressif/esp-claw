local twai = require("twai")
local board_manager = require("board_manager")
local display = require("display")
local system = require("system")

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

local tx_gpio = integer_arg(assert(a.tx_gpio, "tx_gpio is required"), "tx_gpio", 0)
local rx_gpio = integer_arg(assert(a.rx_gpio, "rx_gpio is required"), "rx_gpio", 0)
local bitrate = integer_arg(a.bitrate or 500000, "bitrate", 1)
local mode = a.mode or "normal"
local extended = a.extended == true
local single_shot = a.single_shot ~= false
local can_id = integer_arg(a.id or 0x123, "id", 0, extended and 0x1FFFFFFF or 0x7FF)
local interval_ms = integer_arg(a.interval_ms or 1000, "interval_ms", 10)
-- This firmware builds Lua with 32-bit signed integers. Keep the seed in the
-- representable range, then carry the counter as bytes so it can still wrap
-- across the complete unsigned 32-bit payload range.
local initial_value = integer_arg(a.initial_value or 1, "initial_value", 0, 0x7FFFFFFF)

local bus
local display_initialized = false

local function counter_bytes(value)
    return {
        (value // 0x1000000) % 0x100,
        (value // 0x10000) % 0x100,
        (value // 0x100) % 0x100,
        value % 0x100,
    }
end

local counter = counter_bytes(initial_value)

local function counter_data()
    return { 0, 0, 0, 0, counter[1], counter[2], counter[3], counter[4] }
end

local function increment_counter()
    for i = #counter, 1, -1 do
        counter[i] = (counter[i] + 1) % 0x100
        if counter[i] ~= 0 then
            return
        end
    end
end

local function data_hex(data, dlc)
    local parts = {}
    local first = math.max(1, dlc - 3)
    for i = first, dlc do
        parts[#parts + 1] = string.format("%02X", data[i] or 0)
    end
    return table.concat(parts)
end

local function render(tx_line, rx_line)
    local line_height = 42
    local top = (display.height - line_height * 2) // 2

    display.begin_frame({ clear = true, color = "#101214", preserve = false })
    display.draw_text_aligned(0, top, display.width, line_height, tx_line, {
        color = "#58D68D",
        font_size = 22,
        align = "center",
        valign = "middle",
    })
    display.draw_text_aligned(0, top + line_height, display.width, line_height, rx_line, {
        color = "#5DADE2",
        font_size = 22,
        align = "center",
        valign = "middle",
    })
    display.present()
    display.end_frame()
end

local function cleanup()
    if display_initialized then
        pcall(display.end_frame)
        pcall(display.deinit)
        display_initialized = false
    end
    if bus then
        pcall(function()
            bus:close()
        end)
        bus = nil
    end
end

local function run()
    bus = twai.new({
        tx_gpio = tx_gpio,
        rx_gpio = rx_gpio,
        bitrate = bitrate,
        mode = mode,
        tx_queue_len = 8,
        rx_queue_len = 32,
    })

    local panel_handle, io_handle, width, height, panel_if, pixel_format =
        board_manager.get_display_lcd_params("display_lcd")
    assert(panel_handle, "display_lcd is unavailable: " .. tostring(io_handle))
    display.init(panel_handle, io_handle, width, height, panel_if, pixel_format)
    display_initialized = true
    display.backlight(true)

    local tx_line = "TX:--"
    local rx_line = "RX:--"
    local next_tx_ms = 0
    local next_status_ms = 0
    render(tx_line, rx_line)

    while true do
        local changed = false
        local now = system.millis()

        if mode ~= "listen_only" and now >= next_tx_ms then
            local data = counter_data()
            local ok, send_err = bus:send(can_id, data, {
                extended = extended,
                single_shot = single_shot,
                timeout_ms = 20,
            })
            tx_line = "TX:" .. data_hex(data, 8)
            changed = true
            if ok then
                increment_counter()
            else
                print("[can_lcd_monitor] TX failed: " .. tostring(send_err))
            end
            next_tx_ms = now + interval_ms
        end

        local frame, receive_err = bus:receive(20)
        if frame then
            rx_line = "RX:" .. data_hex(frame.data, frame.dlc)
            changed = true
        elseif receive_err ~= "timeout" then
            print("[can_lcd_monitor] RX failed: " .. tostring(receive_err))
        end

        now = system.millis()
        if now >= next_status_ms then
            local status = bus:status()
            if status.state == "bus_off" or status.state == "recovering" then
                local recovered, recover_err = bus:recover(1000)
                if not recovered then
                    print("[can_lcd_monitor] recovery failed: " .. tostring(recover_err))
                end
            end
            next_status_ms = now + 250
        end

        if changed then
            render(tx_line, rx_line)
        end
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    print("[can_lcd_monitor] ERROR: " .. tostring(err))
    error(err)
end
