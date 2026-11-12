-- display_shape library: spans, safe areas and band placement for each panel outline; no hardware is accessed.
local display_shape = require("display_shape")

local function rejects(fn, text)
    local ok, err = pcall(fn)
    assert(not ok, "expected rejection")
    if text then assert(string.find(tostring(err), text, 1, true), tostring(err)) end
end

local function span_is(frame, y, h, ex, ew)
    local x, w = frame:span(y, h)
    assert(x == ex and w == ew, string.format("span(%d, %d) = %d, %d; want %d, %d", y, h, x, w, ex, ew))
end

-- Rectangle: every band is full width and the safe area is the screen.
local rect = display_shape.new(320, 240, {shape = "rect"})
assert(rect.shape == "rect" and rect.radius == 0)
span_is(rect, 0, 240, 0, 320)
local sx, sy, sw, sh = rect:safe_rect()
assert(sx == 0 and sy == 0 and sw == 320 and sh == 240)
assert(rect:top_for(320, 40) == 0 and rect:bottom_for(320, 40) == 200)
assert(rect:top_for(321, 40) == nil)

-- Circle: bands narrow toward the poles; the safe area is the inscribed square.
local circle = display_shape.new(240, 240, {shape = "circle"})
assert(circle.radius == 120)
span_is(circle, 120, 0, 0, 240)
span_is(circle, 100, 40, 2, 236)
span_is(circle, 8, 32, 77, 86)
span_is(circle, -1, 10, 120, 0)
sx, sy, sw, sh = circle:safe_rect()
assert(sx == 36 and sy == 36 and sw == 168 and sh == 168)
local top = circle:top_for(160, 30)
local _, top_w = circle:span(top, 30)
local _, above_w = circle:span(top - 1, 30)
assert(top_w >= 160 and above_w < 160, "top_for must be the highest fitting band")
local bottom = circle:bottom_for(160, 30)
assert(bottom + 30 == 240 - top, "circle placement must be symmetric")
assert(circle:top_for(241, 10) == nil and circle:top_for(200, 200) == nil)

-- Non-square circle: the shorter side is the diameter, centered on the screen.
local tall = display_shape.new(240, 280, {shape = "circle"})
span_is(tall, 0, 20, 120, 0)
sx, sy = tall:safe_rect()
assert(sx == 36 and sy == 56)

-- Rounded rectangle: only corner zones shrink; the radius is clamped to half the shorter side.
local rounded = display_shape.new(480, 480, {shape = "rounded_rect", corner_radius = 40})
span_is(rounded, 40, 400, 0, 480)
span_is(rounded, 0, 10, 40, 400)
local cx, cw = rounded:span(4, 40)
assert(cx > 0 and cx < 40 and cw == 480 - cx * 2)
sx, sy, sw = rounded:safe_rect()
assert(sx == 12 and sy == 12 and sw == 456)
assert(rounded:top_for(400, 40) == 0 and rounded:top_for(480, 40) == 40)
assert(display_shape.new(100, 60, {shape = "rounded_rect", corner_radius = 90}).radius == 30)

-- Without an override the board shape is used; any valid shape is accepted here.
local board = display_shape.new(240, 240)
assert(board.shape == "rect" or board.shape == "rounded_rect" or board.shape == "circle")

rejects(function() display_shape.new(0, 240) end, "width must be a positive integer")
rejects(function() display_shape.new(240, 240, {shape = "hexagon"}) end, "unknown shape")
rejects(function() display_shape.new(240, 240, {shape = "rounded_rect"}) end, "corner_radius")
print("[display_shape_geometry] passed; rect, circle and rounded_rect outlines")
