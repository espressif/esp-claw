local uart = require("uart")
local delay = require("delay")

local PORT = 1
local TX_GPIO = 14
local RX_GPIO = 13
local BAUD_RATE = 115200

local function close_uart(u)
    if not u then
        return
    end

    local ok, err = pcall(u.close, u)
    if not ok then
        print("[basic_uart] WARN: close failed: " .. tostring(err))
    end
end

local function main()
    print(string.format(
        "[basic_uart] open UART%d tx=%d rx=%d baud=%d",
        PORT, TX_GPIO, RX_GPIO, BAUD_RATE))

    local ok, u = pcall(uart.new, PORT, TX_GPIO, RX_GPIO, BAUD_RATE)
    if not ok then
        print("[basic_uart] ERROR: uart.new failed: " .. tostring(u))
        return
    end

    local success, err = pcall(u.flush_input, u)
    if not success then
        print("[basic_uart] ERROR: flush_input failed: " .. tostring(err))
        close_uart(u)
        return
    end

    local request = "AT\r\n"
    success, err = pcall(u.write, u, request)
    if not success then
        print("[basic_uart] ERROR: write failed: " .. tostring(err))
        close_uart(u)
        return
    end
    print("[basic_uart] sent request: " .. request:gsub("[\r\n]+", "\\r\\n"))

    delay.delay_ms(50)

    local ok_line, reply = pcall(u.read_line, u, 128, 500)
    if not ok_line then
        print("[basic_uart] ERROR: read_line failed: " .. tostring(reply))
        close_uart(u)
        return
    end

    if #reply > 0 then
        local trimmed = reply:gsub("[\r\n]+$", "")
        print("[basic_uart] received line: " .. trimmed)
    else
        print("[basic_uart] no line received within 500 ms")
    end

    print("[basic_uart] polling RX buffer for 2 seconds...")
    for _ = 1, 100 do
        local ok_avail, available = pcall(u.available, u)
        if not ok_avail then
            print("[basic_uart] ERROR: available failed: " .. tostring(available))
            close_uart(u)
            return
        end

        if available > 0 then
            local read_len = available
            if read_len > 64 then
                read_len = 64
            end

            local ok_chunk, chunk = pcall(u.read, u, read_len, 0)
            if not ok_chunk then
                print("[basic_uart] ERROR: read failed: " .. tostring(chunk))
                close_uart(u)
                return
            end

            if #chunk > 0 then
                print(string.format(
                    "[basic_uart] rx chunk (%d bytes): %q",
                    #chunk, chunk))
            end
        end

        delay.delay_ms(20)
    end

    close_uart(u)
    print("[basic_uart] done")
end

main()
