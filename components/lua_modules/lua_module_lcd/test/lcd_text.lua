local lcd = require("lcd")
local display = require("display")
local delay = require("delay")

local ok_gpio, gpio = pcall(require, "gpio")

local BACKLIGHT_GPIO = 16
local BACKLIGHT_ON_LEVEL = 1
local HOLD_MS = 10000

local function enable_backlight()
    if not ok_gpio then
        return
    end

    gpio.set_direction(BACKLIGHT_GPIO, "output")
    gpio.set_level(BACKLIGHT_GPIO, BACKLIGHT_ON_LEVEL)
end

local function cleanup(dev, frame_active)
    if frame_active then
        pcall(display.end_frame)
    end
    pcall(display.deinit)
    if dev then
        pcall(lcd.delete, dev)
    end
end

enable_backlight()

local dev, panel_handle, io_handle, width, height, panel_if = lcd.new({
    controller = "st7789",
    bus = {
        host = 2,
        mode = "spi",
        sclk = 4,
        mosi = 5,
        max_transfer_sz = 6400,
    },
    io = {
        cs = 15,
        dc = 7,
        spi_mode = 0,
        pclk_hz = 40000000,
    },
    panel = {
        reset = 6,
        mirror_x = false,
        mirror_y = true,
        swap_xy = true,
        invert_color = true,
    },
    resolution = {
        width = 320,
        height = 240,
    }
})

local info = lcd.get_info(dev)
print(string.format("[lcd_text] controller=%s size=%dx%d host=%d mode=%s",
    info.controller, info.width, info.height, info.host, info.bus_mode))

assert(display.init(panel_handle, io_handle, width, height, panel_if))

local frame_active = false
local ok, err = pcall(function()
    display.begin_frame({ clear = true, color = { r = 8, g = 18, b = 36 } })
    frame_active = true

    display.draw_text(20, 32, "ESP-Claw LCD Demo", {
        color = { r = 255, g = 240, b = 120 },
        font_size = 24,
    })

    display.draw_text(20, 74, "Board: esp32_S3_DevKitC_1_breadboard", {
        color = { r = 220, g = 230, b = 255 },
        font_size = 14,
    })

    display.draw_text(20, 102, "Panel: ST7789 SPI 320x240", {
        color = { r = 120, g = 255, b = 180 },
        font_size = 18,
    })

    display.draw_text(20, 132, "Hello from Lua", {
        color = "white",
        font_size = 20,
    })

    display.draw_text(20, 160, "Pins: SCLK=4 MOSI=5 CS=15 DC=7 RST=6 BL=16", {
        color = { r = 180, g = 190, b = 210 },
        font_size = 12,
    })

    display.draw_rect(12, 20, width - 24, height - 40, { r = 90, g = 170, b = 255 })
    display.present()
    delay.delay_ms(HOLD_MS)
end)

cleanup(dev, frame_active)

if not ok then
    error(err)
end

print("[lcd_text] done")
