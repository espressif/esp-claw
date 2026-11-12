-- Visible-area geometry for rectangular, rounded-rectangle and circular panels; pure math, no drawing.
local M = {}
local Frame = {}
Frame.__index = Frame

local SHAPES = {rect = true, rounded_rect = true, circle = true}
local SAFE_INSET_RATIO = 1 - 1 / math.sqrt(2)

local function fail(fmt, ...) error("display_shape: " .. string.format(fmt, ...), 0) end

local function check_size(value, name)
    if type(value) ~= "number" or value <= 0 or value % 1 ~= 0 then fail("%s must be a positive integer", name) end
end

local function board_shape()
    local ok, board_manager = pcall(require, "board_manager")
    if not ok or type(board_manager) ~= "table" or type(board_manager.get_display_shape) ~= "function" then return {} end
    local info = board_manager.get_display_shape()
    return type(info) == "table" and info or {}
end

-- The visible area is modelled as one rounded rectangle (ox, oy, rw, rh, r); a circle is a square with r = rw / 2.
function M.new(width, height, opts)
    check_size(width, "width")
    check_size(height, "height")
    if opts ~= nil and type(opts) ~= "table" then fail("opts must be a table") end
    opts = opts or {}
    local source = opts.shape and opts or board_shape()
    local shape = source.shape or "rect"
    if not SHAPES[shape] then fail("unknown shape '%s'", tostring(shape)) end
    local self = setmetatable({shape = shape, width = width, height = height, ox = 0, oy = 0, rw = width, rh = height, radius = 0}, Frame)
    if shape == "circle" then
        local d = math.min(width, height)
        self.ox, self.oy, self.rw, self.rh, self.radius = (width - d) // 2, (height - d) // 2, d, d, d / 2
    elseif shape == "rounded_rect" then
        local radius = source.corner_radius
        if type(radius) ~= "number" or radius < 0 then fail("corner_radius must be a non-negative number") end
        self.radius = math.min(radius, math.min(width, height) / 2)
    end
    return self
end

-- Half of the visible width on the horizontal line y.
local function half_width(self, y)
    local top, bottom, r = self.oy, self.oy + self.rh, self.radius
    if y < top or y > bottom then return 0 end
    local dy = math.max(top + r - y, y - (bottom - r), 0)
    return self.rw / 2 - r + math.sqrt(math.max(r * r - dy * dy, 0))
end

-- Vertical distance from a corner arc's straight edge at which the visible width reaches w; nil when w never fits.
local function reach(self, w)
    local s = w / 2 - (self.rw / 2 - self.radius)
    if s <= 0 then return self.radius end
    if s > self.radius then return nil end
    return math.sqrt(self.radius * self.radius - s * s)
end

-- Usable horizontal span for the band [y, y + h): both edges are checked because the outline is monotonic per corner.
function Frame:span(y, h)
    local half = math.min(half_width(self, y), half_width(self, y + h))
    local center = self.ox + self.rw / 2
    local x = math.ceil(center - half)
    return x, math.max(0, math.floor(center + half) - x)
end

-- Largest centered axis-aligned rectangle that touches each corner arc at 45 degrees.
function Frame:safe_rect()
    local inset = math.ceil(self.radius * SAFE_INSET_RATIO)
    return self.ox + inset, self.oy + inset, self.rw - inset * 2, self.rh - inset * 2
end

-- Smallest y whose band [y, y + h) is at least w wide, or nil.
function Frame:top_for(w, h)
    local dy = reach(self, w)
    if not dy then return nil end
    local y = math.ceil(self.oy + self.radius - dy)
    local _, span_w = self:span(y, h)
    return span_w >= w and y or nil
end

-- Largest y whose band [y, y + h) is at least w wide, or nil.
function Frame:bottom_for(w, h)
    local dy = reach(self, w)
    if not dy then return nil end
    local y = math.floor(self.oy + self.rh - self.radius + dy) - h
    local _, span_w = self:span(y, h)
    return span_w >= w and y or nil
end

return M
