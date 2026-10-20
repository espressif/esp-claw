local display = require("display")
local path = "/tmp/display-font-contract.dfn"
local header = string.pack("<c4i2i2i2I2I4I4", "DFN1", 7, 0, 6, 0, 1, 65)
local record = string.pack("<I4I4i2i2i2I2I2", 65, 0, 6, 0, 0, 5, 7)
local bitmap = string.char(0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88)
local function write(data)
    local file <close> = assert(io.open(path, "wb"))
    assert(file:write(data))
end
local function rejected(fn)
    assert(not pcall(fn), "expected font argument or data rejection")
end

write(header .. record .. bitmap)
local screen <close> = display.open()
local font <close> = display.load_font(path)
local w, h = screen:measure_text("A", { font = font })
assert(w == 6 and h == 7)
local fw, fh = screen:measure_text("?", { font = font })
assert(fw == w and fh == h)
rejected(function() screen:measure_text("A", { font = {} }) end)
screen:begin({ clear = "#101D22" })
screen:text(10, 10, "A?", { font = font, color = "#7CE6AC" })
screen:present()
font:close()
font:close()
rejected(function() screen:measure_text("A", { font = font }) end)
for _ = 1, 16 do
    local temporary <close> = display.load_font(path)
    assert(screen:measure_text("A", { font = temporary }) == 6)
end
-- Truncation must fail without leaving a live font handle.
write(header .. record)
rejected(function() display.load_font(path) end)
os.remove(path)
print("DISPLAY_FONT_CONTRACT_OK")
