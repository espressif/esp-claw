local display = require("display")
local image = require("image")
local options = type(args) == "table" and args or {}
local screen <close> = display.open()
local info = screen:info()
local frame <close> = image.load_file(options.image_path or "/system/apps/dino/assets/icon.jpg")
local frame_info = frame:info()
local width, height = math.min(48, info.width), math.min(48, info.height)
local checks = 0

local function rejects(fn, ...)
    assert(not pcall(fn, ...), "expected an error")
    checks = checks + 1
end

screen:begin({ clear = 0xFF000000 })
assert(screen:present())
for _, mode in ipairs({"raw", "contain", "cover", "stretch", "crop"}) do
    screen:begin()
    screen:image(0, 0, frame, { mode = mode, width = width, height = height, opacity = 192,
        source = { x = 0, y = 0, width = math.min(24, frame_info.width), height = math.min(16, frame_info.height) } })
    assert(screen:present(), "image mode produced no dirty pixels: " .. mode)
    print(string.format("[display_image_contract] mode=%s pixels=%d", mode, screen:stats().dirty_pixels))
end

screen:begin()
screen:image(0, 0, frame, { opacity = 0 })
assert(not screen:present(), "zero opacity submitted pixels")
screen:begin()
rejects(screen.image, screen, 0, 0, frame, { mode = "fit" })
rejects(screen.image, screen, 0, 0, frame, { source = { x = frame_info.width, width = 1 } })
rejects(screen.image, screen, 0, 0, frame, { opacity = 256 })
rejects(screen.image, screen, 0, 0, {}, {})
rejects(screen.blit, screen, 0, 0, "\0", { width = 1, height = 1, format = "rgb565" })
rejects(screen.blit, screen, 0, 0, "\0\0\0", { width = 1, height = 1, format = "rgb565" })
rejects(screen.blit, screen, 0, 0, "", { width = 0x7FFFFFFF, height = 0x7FFFFFFF, format = "rgb888" })
screen:blit(0, 0, "\0\248", { width = 1, height = 1, format = "rgb565" })
screen:blit(1, 0, "\255\0\0", { width = 1, height = 1, format = "rgb888" })
screen:blit(2, 0, "\0\0\255", { width = 1, height = 1, format = "bgr888" })
assert(screen:present())
frame:release()
screen:begin()
rejects(screen.image, screen, 0, 0, frame, {})
assert(not screen:present(), "failed image draw dirtied frame")
print(string.format("[display_image_contract] PASS checks=%d; first three pixels should all be red", checks))
