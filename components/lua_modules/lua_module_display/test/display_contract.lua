local display = require("display")
local system = require("system")
local options = type(args) == "table" and args or {}
local checks = 0

local function rejects(fn, ...)
    local ok, err = pcall(fn, ...)
    assert(not ok, "expected an error")
    checks = checks + 1
    return tostring(err)
end

local function open(count)
    return display.open({ framebuffer_count = count or 1 })
end

for _, name in ipairs({"init", "deinit", "begin_frame", "end_frame", "draw_text", "touch", "backlight"}) do
    assert(display[name] == nil, "legacy API exposed: " .. name)
end
assert(display.color(1, 2, 3) == 0xFF010203)
assert(display.color(1, 2, 3, 4) == 0x04010203)
rejects(display.color, 256, 0, 0)
rejects(display.open, { framebuffer_count = 3 })

for count = 1, 2 do
    local screen <close> = open(count)
    local info = screen:info()
    assert(info.width >= 4 and info.height >= 4)
    assert(info.pixel_format == "rgb565" and info.rgb565_swap == nil)
    assert(info.framebuffer_count == count)
    assert(info.framebuffer_bytes == info.width * info.height * info.bytes_per_pixel * count)
    assert(rejects(open):find("already open", 1, true))
    rejects(screen.present, screen)
    rejects(screen.fill_rect, screen, 0, 0, 1, 1, 0xFFFFFFFF)
    rejects(screen.save, screen)

    screen:begin({ clear = 0xFF000000 })
    rejects(screen.begin, screen)
    rejects(screen.restore, screen)
    rejects(screen.fill_rect, screen, 0.5, 0, 1, 1, 0xFFFFFFFF)
    rejects(screen.fill_rect, screen, 0, 0, 1, 1, "white")
    local depth = 0
    for _ = 1, 64 do
        if not pcall(screen.save, screen) then break end
        depth = depth + 1
    end
    assert(depth > 0 and depth < 64, "state stack must be bounded")
    for _ = 1, depth do screen:restore() end
    rejects(screen.restore, screen)
    assert(screen:present())
    assert(screen:stats().dirty_pixels == info.width * info.height)

    screen:begin()
    screen:fill_rect(0, 0, 2, 2, 0x00010203)
    assert(not screen:present(), "transparent draw submitted pixels")
    assert(screen:stats().dirty_pixels == 0)
    screen:begin()
    screen:fill_rect(-2, -2, 4, 4, 0x80FF0000)
    assert(screen:present())
    assert(screen:stats().dirty_pixels == 4, "negative clipping dirty area")

    screen:begin()
    screen:fill_rect(0, 0, 2, 2, 0xFFFFFFFF)
    screen:fill_rect(info.width - 2, info.height - 2, 2, 2, 0xFFFFFFFF)
    assert(screen:present())
    assert(screen:stats().dirty_pixels == 8, "separate dirty regions were expanded")

    screen:begin()
    screen:save()
    screen:translate(1, 1)
    screen:clip(0, 0, 2, 2)
    screen:fill_rect(-100, -100, 200, 200, {r=0,g=255,b=0,a=128})
    screen:restore()
    assert(screen:present())
    assert(screen:stats().dirty_pixels == 4, "translated clip dirty area")
    screen:begin()
    assert(screen:present({ full = true }))
    local stats = screen:stats()
    assert(stats.dirty_pixels == info.width * info.height and stats.dirty_rects == 1)
    assert(stats.submitted_bytes == stats.dirty_pixels * info.bytes_per_pixel)
    assert(stats.draw_us >= stats.sync_us and stats.present_us >= 0)
    assert(stats.framebuffer_bytes == info.framebuffer_bytes)
    screen:begin()
    assert(not screen:present(), "empty frame submitted pixels")

    if info.touch_available then
        local touch = screen:touch()
        assert(type(touch.points) == "table")
        for _, point in ipairs(touch.points) do
            assert(math.type(point.x) == "integer" and math.type(point.y) == "integer" and math.type(point.id) == "integer")
        end
    else
        assert(rejects(screen.touch, screen):find("NOT_SUPPORTED", 1, true))
    end
    screen:close()
    screen:close()
    assert(rejects(screen.info, screen):find("closed", 1, true))
    rejects(screen.begin, screen)
    rejects(screen.touch, screen)
    print(string.format("[display_contract] buffers=%d frame_bytes=%d checks=%d", count, stats.framebuffer_bytes, checks))
end

-- Scope errors must release the screen before the next open.
rejects(function()
    local screen <close> = open()
    screen:begin()
    error("intentional scope failure")
end)

collectgarbage("collect")
local before = system.heap.get_info(system.heap.caps.DEFAULT).free_size
local cycles = math.tointeger(options.cycles or 100)
assert(cycles and cycles > 0 and cycles <= 1000)
for i = 1, cycles do
    do
        local screen <close> = open()
        screen:begin({ clear = 0xFF101820 })
        screen:fill_rect(0, 0, 4, 4, 0xFFFFFFFF)
        assert(screen:present())
    end
    if i % 10 == 0 then collectgarbage("collect") end
end
collectgarbage("collect")
local after = system.heap.get_info(system.heap.caps.DEFAULT).free_size
print(string.format("[display_contract] PASS cycles=%d checks=%d heap_before=%d heap_after=%d delta=%d", cycles, checks, before, after, after-before))
