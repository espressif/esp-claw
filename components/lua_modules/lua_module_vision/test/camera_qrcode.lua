local camera = require("camera")
local delay = require("delay")
local display = require("display")
local screen, screen_info
local image = require("image")
local qrcode = require("qrcode_detect")

local TAG = "[camera_qrcode]"
local RUN_SECONDS = 30
local FRAME_PERIOD_MS = 100
local CAPTURE_TIMEOUT_MS = 3000
local CAMERA_OPEN_OPTS = { format = { "JPEG", "RGBP", "YUYV", "UYVY", "YU12" }, width = 320, height = 240, nearest = true, }

local display_started = false
local camera_started = false

local function clamp(value, min_value, max_value)
    return math.max(min_value, math.min(value, max_value))
end

local function draw_codes(result, image_w, image_h)
    local scale = math.min(screen_info.width / image_w, screen_info.height / image_h)
    local draw_w = math.floor(image_w * scale)
    local draw_h = math.floor(image_h * scale)
    local ox, oy = (screen_info.width - draw_w) // 2, (screen_info.height - draw_h) // 2

    -- Match screen:image(..., mode = "contain").
    for i = 1, result.count do
        local code = result[i]
        local x1 = clamp(ox + math.floor(code.left * draw_w / image_w), 0, screen_info.width - 1)
        local y1 = clamp(oy + math.floor(code.top * draw_h / image_h), 0, screen_info.height - 1)
        local x2 = clamp(ox + math.floor((code.right + 1) * draw_w / image_w) - 1, 0, screen_info.width - 1)
        local y2 = clamp(oy + math.floor((code.bottom + 1) * draw_h / image_h) - 1, 0, screen_info.height - 1)
        local width = x2 - x1 + 1
        local height = y2 - y1 + 1
        if width > 0 and height > 0 then
            screen:stroke_rect(x1, y1, width, height, { r = 80, g = 255, b = 80 })
            if width > 4 and height > 4 then
                screen:stroke_rect(x1 + 1, y1 + 1, width - 2, height - 2, { r = 255, g = 255, b = 64 })
            end
        end
    end
end

local function draw_status(frames, decoded, count)
    local found = count > 0
    screen:fill_rect(0, 0, screen_info.width, 48, found and { r = 24, g = 112, b = 48 } or { r = 24, g = 24, b = 24 })
    screen:text(8, 6, found and "QR: FOUND" or "QR: SEARCH", { color = "#ffffff", font_size = 16 })
    screen:text(8, 28, string.format("found=%d decoded=%d frame=%d", count, decoded, frames), { color = "#ffffff", font_size = 12 })
end

local function cleanup()
    if display_started then
        pcall(screen.close, screen)
        display_started = false
    end
    if camera_started then
        pcall(camera.close)
        camera_started = false
    end
end

local devices = camera.list_devices()
if #devices == 0 then
    print(TAG .. " SKIP: no capture video device available")
    return
end

local ok, err = pcall(display.open)
if not ok then
    print(TAG .. " SKIP: display.open failed: " .. tostring(err))
    return
end
screen = err
screen_info = screen:info()
display_started = true

ok, err = pcall(camera.open, devices[1].path, CAMERA_OPEN_OPTS)
if not ok then
    print(TAG .. " SKIP: camera.open failed: " .. tostring(err))
    cleanup()
    return
end
camera_started = true

local run_ok, run_err = xpcall(function()
    local stream = camera.info()
    local deadline_s = os.time() + RUN_SECONDS
    local frames = 0
    local decoded = 0
    local ticker = delay.periodic(FRAME_PERIOD_MS)

    print(string.format("%s start stream=%dx%d format=%s", TAG, stream.width, stream.height, tostring(stream.pixel_format)))
    while os.time() < deadline_s do
        local frame <close> = camera.get_frame(CAPTURE_TIMEOUT_MS)
        local result = qrcode.detect(frame)
        local rgb565 <close> = image.convert(frame, image.RGB565)
        local frame_info = rgb565:info()
        frames = frames + 1
        decoded = decoded + result.count

        screen:begin({ clear = "#000000" })
        screen:image(0, 0, rgb565, { mode = "contain", width = screen_info.width, height = screen_info.height })
        draw_codes(result, frame_info.width, frame_info.height)
        draw_status(frames, decoded, result.count)
        screen:present()

        for i = 1, result.count do
            local code = result[i]
            print(string.format("%s frame=%d payload=%q box=%d,%d,%d,%d", TAG, frames, code.payload,
                code.left, code.top, code.right, code.bottom))
        end
        if frames == 1 or frames % 10 == 0 then
            print(string.format("%s frame=%d found=%d total_decoded=%d", TAG, frames, result.count, decoded))
        end
        ticker:wait()
    end
    print(string.format("%s PASS frames=%d decoded=%d", TAG, frames, decoded))
end, debug.traceback)

cleanup()
if not run_ok then
    error(run_err)
end
