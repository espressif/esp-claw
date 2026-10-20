local display = require("display")
local delay = require("delay")
local screen <close> = display.open()
local info = screen:info()
local width, height = math.min(96, info.width), math.min(64, info.height)
local rows = {}

-- Raw RGB565 strings contain native little-endian pixels.
for y = 0, height - 1 do
    local row = {}
    for x = 0, width - 1 do
        local r, g = x * 31 // math.max(1, width - 1), y * 63 // math.max(1, height - 1)
        local pixel = (r << 11) | (g << 5) | 16
        row[#row + 1] = string.pack("<I2", pixel)
    end
    rows[#rows + 1] = table.concat(row)
end
local pixels = table.concat(rows)
local blit_options = { width = width, height = height, format = "rgb565" }

screen:begin({ clear = 0xFF101820 })
assert(not pcall(screen.blit, screen, 0, 0, pixels:sub(1, 16), blit_options), "short buffer accepted")
screen:blit(0, 0, pixels, blit_options)
screen:save()
screen:translate(width // 2, height // 2)
screen:clip(0, 0, width // 2, height // 2)
screen:blit(0, 0, pixels, blit_options)
screen:restore()
assert(screen:present())
screen:begin()
assert(not screen:present(), "empty frame submitted pixels")
delay.delay_ms(1200)
print("[display_pixels_demo] PASS")
