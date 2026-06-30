local SSD1306 = {}
SSD1306.__index = SSD1306

local CMD_CONTROL = 0x00
local DATA_CONTROL = 0x40
local DEFAULT_WIDTH = 128
local DEFAULT_HEIGHT = 64
local DEFAULT_ADDR = 0x3C
local MAX_I2C_PAYLOAD = 31

local FONT = {
    [" "] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ["!"] = {0x00, 0x00, 0x5F, 0x00, 0x00},
    ["\""] = {0x00, 0x07, 0x00, 0x07, 0x00},
    ["#"] = {0x14, 0x7F, 0x14, 0x7F, 0x14},
    ["$"] = {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    ["%"] = {0x23, 0x13, 0x08, 0x64, 0x62},
    ["&"] = {0x36, 0x49, 0x56, 0x20, 0x50},
    ["'"] = {0x00, 0x08, 0x07, 0x03, 0x00},
    ["("] = {0x00, 0x1C, 0x22, 0x41, 0x00},
    [")"] = {0x00, 0x41, 0x22, 0x1C, 0x00},
    ["*"] = {0x2A, 0x1C, 0x7F, 0x1C, 0x2A},
    ["+"] = {0x08, 0x08, 0x3E, 0x08, 0x08},
    [","] = {0x00, 0x80, 0x70, 0x30, 0x00},
    ["-"] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ["."] = {0x00, 0x00, 0x00, 0x18, 0x18},
    ["/"] = {0x20, 0x10, 0x08, 0x04, 0x02},
    ["0"] = {0x3E, 0x51, 0x49, 0x45, 0x3E},
    ["1"] = {0x00, 0x42, 0x7F, 0x40, 0x00},
    ["2"] = {0x42, 0x61, 0x51, 0x49, 0x46},
    ["3"] = {0x21, 0x41, 0x45, 0x4B, 0x31},
    ["4"] = {0x18, 0x14, 0x12, 0x7F, 0x10},
    ["5"] = {0x27, 0x45, 0x45, 0x45, 0x39},
    ["6"] = {0x3C, 0x4A, 0x49, 0x49, 0x30},
    ["7"] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ["8"] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ["9"] = {0x06, 0x49, 0x49, 0x29, 0x1E},
    [":"] = {0x00, 0x36, 0x36, 0x00, 0x00},
    [";"] = {0x00, 0x40, 0x36, 0x36, 0x00},
    ["<"] = {0x08, 0x14, 0x22, 0x41, 0x00},
    ["="] = {0x14, 0x14, 0x14, 0x14, 0x14},
    [">"] = {0x00, 0x41, 0x22, 0x14, 0x08},
    ["?"] = {0x02, 0x01, 0x51, 0x09, 0x06},
    ["@"] = {0x3E, 0x41, 0x5D, 0x59, 0x4E},
    ["A"] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
    ["B"] = {0x7F, 0x49, 0x49, 0x49, 0x36},
    ["C"] = {0x3E, 0x41, 0x41, 0x41, 0x22},
    ["D"] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
    ["E"] = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ["F"] = {0x7F, 0x09, 0x09, 0x09, 0x01},
    ["G"] = {0x3E, 0x41, 0x41, 0x51, 0x73},
    ["H"] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
    ["I"] = {0x00, 0x41, 0x7F, 0x41, 0x00},
    ["J"] = {0x20, 0x40, 0x41, 0x3F, 0x01},
    ["K"] = {0x7F, 0x08, 0x14, 0x22, 0x41},
    ["L"] = {0x7F, 0x40, 0x40, 0x40, 0x40},
    ["M"] = {0x7F, 0x02, 0x1C, 0x02, 0x7F},
    ["N"] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
    ["O"] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
    ["P"] = {0x7F, 0x09, 0x09, 0x09, 0x06},
    ["Q"] = {0x3E, 0x41, 0x51, 0x21, 0x5E},
    ["R"] = {0x7F, 0x09, 0x19, 0x29, 0x46},
    ["S"] = {0x46, 0x49, 0x49, 0x49, 0x31},
    ["T"] = {0x01, 0x01, 0x7F, 0x01, 0x01},
    ["U"] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ["V"] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
    ["W"] = {0x7F, 0x20, 0x18, 0x20, 0x7F},
    ["X"] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ["Y"] = {0x03, 0x04, 0x78, 0x04, 0x03},
    ["Z"] = {0x61, 0x51, 0x49, 0x45, 0x43},
    ["["] = {0x00, 0x7F, 0x41, 0x41, 0x41},
    ["\\"] = {0x02, 0x04, 0x08, 0x10, 0x20},
    ["]"] = {0x41, 0x41, 0x41, 0x7F, 0x00},
    ["^"] = {0x04, 0x02, 0x01, 0x02, 0x04},
    ["_"] = {0x40, 0x40, 0x40, 0x40, 0x40},
    ["`"] = {0x00, 0x03, 0x07, 0x08, 0x00},
    ["{"] = {0x08, 0x36, 0x41, 0x41, 0x00},
    ["|"] = {0x00, 0x00, 0x7F, 0x00, 0x00},
    ["}"] = {0x00, 0x41, 0x41, 0x36, 0x08},
    ["~"] = {0x02, 0x01, 0x02, 0x04, 0x02},
}

local function validate_dimensions(width, height)
    if width == 128 and (height == 64 or height == 32) then
        return
    end
    error(string.format("ssd1306: unsupported dimensions %dx%d", width, height))
end

local function check_byte(value, name)
    if type(value) ~= "number" or value < 0 or value > 255 then
        error(string.format("ssd1306: %s must be 0-255", name))
    end
    return math.floor(value)
end

local function bytes_to_string(bytes, first, last)
    local parts = {}
    for i = first, last do
        parts[#parts + 1] = string.char(bytes[i])
    end
    return table.concat(parts)
end

local function font_for_char(ch)
    if type(ch) ~= "string" or ch == "" then
        return FONT[" "]
    end
    return FONT[ch] or FONT[string.upper(ch)] or FONT[" "]
end

function SSD1306.new(dev, opts)
    opts = opts or {}
    if not dev or type(dev.write) ~= "function" then
        error("ssd1306: dev must be an i2c device handle")
    end

    local width = opts.width or DEFAULT_WIDTH
    local height = opts.height or DEFAULT_HEIGHT
    validate_dimensions(width, height)

    local self = setmetatable({}, SSD1306)
    self.dev = dev
    self.width = width
    self.height = height
    self.pages = math.floor(height / 8)
    self.addr = opts.addr or DEFAULT_ADDR
    self.external_vcc = opts.external_vcc == true
    self.segment_remap = opts.segment_remap ~= false
    self.com_scan_dec = opts.com_scan_dec ~= false
    self.closed = false
    self.buffer = {}

    for i = 1, width * self.pages do
        self.buffer[i] = 0x00
    end

    return self
end

function SSD1306:_ensure_open()
    if self.closed then
        error("ssd1306: display is closed")
    end
end

function SSD1306:_write_command(...)
    self:_ensure_open()
    local bytes = {...}
    for i = 1, #bytes do
        check_byte(bytes[i], "command byte")
    end
    self.dev:write(bytes_to_string(bytes, 1, #bytes), CMD_CONTROL)
end

function SSD1306:_write_data(page, start_col, length)
    self:_ensure_open()
    self:_write_command(0xB0 + page)
    self:_write_command(start_col & 0x0F, 0x10 + ((start_col >> 4) & 0x0F))

    local base = page * self.width + start_col + 1
    local offset = 0
    while offset < length do
        local chunk = math.min(MAX_I2C_PAYLOAD, length - offset)
        local first = base + offset
        local last = first + chunk - 1
        self.dev:write(bytes_to_string(self.buffer, first, last), DATA_CONTROL)
        offset = offset + chunk
    end
end

function SSD1306:init()
    self:_ensure_open()

    local multiplex = self.height - 1
    local com_pins = (self.height == 64) and 0x12 or 0x02
    local contrast = (self.height == 64) and 0xCF or 0x8F
    local charge_pump = self.external_vcc and 0x10 or 0x14
    local precharge = self.external_vcc and 0x22 or 0xF1

    self:_write_command(
        0xAE,
        0xD5, 0x80,
        0xA8, multiplex,
        0xD3, 0x00,
        0x40,
        0x8D, charge_pump,
        0x20, 0x02,
        self.segment_remap and 0xA1 or 0xA0,
        self.com_scan_dec and 0xC8 or 0xC0,
        0xDA, com_pins,
        0x81, contrast,
        0xD9, precharge,
        0xDB, 0x40,
        0xA4,
        0xA6,
        0x2E,
        0xAF
    )

    return self
end

function SSD1306:clear(color)
    self:_ensure_open()
    local fill = color and 0xFF or 0x00
    for i = 1, #self.buffer do
        self.buffer[i] = fill
    end
end

function SSD1306:pixel(x, y, color)
    self:_ensure_open()
    if x < 0 or y < 0 or x >= self.width or y >= self.height then
        return
    end

    local page = math.floor(y / 8)
    local mask = 1 << (y % 8)
    local index = page * self.width + x + 1
    if color then
        self.buffer[index] = self.buffer[index] | mask
    else
        self.buffer[index] = self.buffer[index] & (~mask & 0xFF)
    end
end

function SSD1306:fill_rect(x, y, w, h, color)
    self:_ensure_open()
    if w <= 0 or h <= 0 then
        return
    end
    for yy = y, y + h - 1 do
        for xx = x, x + w - 1 do
            self:pixel(xx, yy, color)
        end
    end
end

function SSD1306:draw_char(x, y, ch, color)
    self:_ensure_open()
    local glyph = font_for_char(type(ch) == "string" and ch:sub(1, 1) or " ")
    local on = color ~= false

    for col = 1, 5 do
        local column_bits = glyph[col]
        for row = 0, 6 do
            local lit = ((column_bits >> row) & 0x01) == 0x01
            if lit then
                self:pixel(x + col - 1, y + row, on)
            elseif not on then
                self:pixel(x + col - 1, y + row, false)
            end
        end
    end

    if not on then
        for row = 0, 6 do
            self:pixel(x + 5, y + row, false)
        end
    end
end

function SSD1306:draw_text(x, y, text, color)
    self:_ensure_open()
    text = tostring(text or "")
    local cursor_x = x
    local cursor_y = y
    local on = color ~= false

    for i = 1, #text do
        local ch = text:sub(i, i)
        if ch == "\n" then
            cursor_x = x
            cursor_y = cursor_y + 8
        else
            self:draw_char(cursor_x, cursor_y, ch, on)
            cursor_x = cursor_x + 6
        end
    end
end

function SSD1306:invert(enable)
    self:_ensure_open()
    self:_write_command(enable and 0xA7 or 0xA6)
end

function SSD1306:contrast(value)
    self:_ensure_open()
    self:_write_command(0x81, check_byte(value, "contrast"))
end

function SSD1306:show()
    self:_ensure_open()
    for page = 0, self.pages - 1 do
        self:_write_data(page, 0, self.width)
    end
end

function SSD1306:close()
    if self.closed then
        return
    end
    pcall(function()
        self:_write_command(0xAE)
    end)
    self.closed = true
end

return {
    new = SSD1306.new,
}
