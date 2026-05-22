local i2c = require("i2c")
local delay = require("delay")

local I2C_PORT = 0
local SDA_GPIO = 14
local SCL_GPIO = 13
local FREQ_HZ = 400000
local OLED_ADDR = 0x3C

local function load_ssd1306_driver()
    local candidates = {
        "/fatfs/scripts/ssd1306.lua",
        "/fatfs/scripts/builtin/ssd1306.lua",
        "ssd1306.lua",
        "builtin/ssd1306.lua",
    }

    local errors = {}
    for _, path in ipairs(candidates) do
        local ok, mod = pcall(dofile, path)
        if ok then
            return mod, path
        end
        errors[#errors + 1] = string.format("%s => %s", path, tostring(mod))
    end

    error("[ssd1306_demo] failed to load SSD1306 driver: " .. table.concat(errors, " | "))
end

local function has_addr(addrs, target)
    for _, addr in ipairs(addrs) do
        if addr == target then
            return true
        end
    end
    return false
end

local function draw_border(display)
    display:fill_rect(0, 0, display.width, 1, true)
    display:fill_rect(0, display.height - 1, display.width, 1, true)
    display:fill_rect(0, 0, 1, display.height, true)
    display:fill_rect(display.width - 1, 0, 1, display.height, true)
end

local function draw_checker(display, x, y, w, h, cell)
    for yy = 0, h - 1 do
        for xx = 0, w - 1 do
            local block = (math.floor(xx / cell) + math.floor(yy / cell)) % 2 == 0
            display:pixel(x + xx, y + yy, block)
        end
    end
end

local bus = nil
local dev = nil
local display = nil

local function cleanup()
    if display then
        pcall(function() display:close() end)
        display = nil
    end
    if dev then
        pcall(function() dev:close() end)
        dev = nil
    end
    if bus then
        pcall(function() bus:close() end)
        bus = nil
    end
end

local ok, err = pcall(function()
    print(string.format(
        "[ssd1306_demo] open I2C port=%d sda=%d scl=%d freq=%d addr=0x%02X",
        I2C_PORT,
        SDA_GPIO,
        SCL_GPIO,
        FREQ_HZ,
        OLED_ADDR
    ))

    bus = i2c.new(I2C_PORT, SDA_GPIO, SCL_GPIO, FREQ_HZ)
    local addrs = bus:scan()
    if #addrs == 0 then
        error("[ssd1306_demo] no I2C device found")
    end
    if not has_addr(addrs, OLED_ADDR) then
        error(string.format("[ssd1306_demo] SSD1306 not found at 0x%02X", OLED_ADDR))
    end

    dev = bus:device(OLED_ADDR)
    print("[ssd1306_demo] i2c device opened")
    local ssd1306, driver_path = load_ssd1306_driver()
    print("[ssd1306_demo] driver loaded from " .. driver_path)
    display = ssd1306.new(dev, {
        width = 128,
        height = 64,
        addr = OLED_ADDR,
    })

    display:init()
    print("[ssd1306_demo] panel initialized")

    display:clear(false)
    display:show()
    print("[ssd1306_demo] clear black")
    delay.delay_ms(700)

    display:clear(true)
    display:show()
    print("[ssd1306_demo] full white")
    delay.delay_ms(700)

    display:invert(true)
    print("[ssd1306_demo] invert on")
    delay.delay_ms(500)
    display:invert(false)
    print("[ssd1306_demo] invert off")

    display:clear(false)
    draw_border(display)
    display:show()
    print("[ssd1306_demo] border")
    delay.delay_ms(900)

    display:clear(false)
    draw_checker(display, 16, 8, 96, 32, 4)
    draw_border(display)
    display:show()
    print("[ssd1306_demo] checkerboard")
    delay.delay_ms(1000)

    display:clear(false)
    draw_border(display)
    display:draw_text(10, 10, "SSD1306 OK", true)
    display:draw_text(10, 26, "128X64 0X3C", true)
    display:draw_text(10, 42, "I2C TEST", true)
    display:show()
    print("[ssd1306_demo] text frame")
    delay.delay_ms(3000)

    print("[ssd1306_demo] done")
end)

cleanup()
if not ok then
    print("[ssd1306_demo] ERROR: " .. tostring(err))
    error(err)
end
