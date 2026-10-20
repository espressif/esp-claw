local camera = require("camera")
local color_detect = require("color_detect")
local delay = require("delay")
local display = require("display")
local screen, screen_info

local function center_text(x, y, w, h, text, options)
    local tw, th = screen:measure_text(text, options)
    screen:text(x + math.max(0, (w - tw) // 2), y + math.max(0, (h - th) // 2), text, options)
end
local image = require("image")

local TAG = "[camera_display_color]"
local RUN_SECONDS = 30
local CAPTURE_TIMEOUT_MS = 3000
local FRAME_PERIOD_MS = 30
-- Whatever the sensor exposes; image.convert covers any of these on output.
local CAMERA_OPEN_OPTS = { format = { "JPEG", "RGBP", "YUYV", "UYVY", "YU12" }, width = 320, height = 240, nearest = true, }
local COLOR_OPTS = {
    -- LAB envelope converted from HSV green {35,100,100}..{85,255,255}.
    l_min = 35,
    l_max = 97,
    a_min = -88,
    a_max = -12,
    b_min = -3,
    b_max = 92,
    x_stride = 2,
    y_stride = 2,
    min_pixels = 250,
    max_blob_pixels = 20000,
}

local display_started = false
local camera_started = false

local function fit_size(src_w, src_h, max_w, max_h)
    local ratio = math.min(max_w / src_w, max_h / src_h)
    return math.max(1, math.floor(src_w * ratio)), math.max(1, math.floor(src_h * ratio))
end

local function clamp(value, min_value, max_value)
    if value < min_value then
        return min_value
    end
    if value > max_value then
        return max_value
    end
    return value
end

local function map_x(value, image_w, draw_w)
    return clamp((screen_info.width - draw_w) // 2 + math.floor(value * draw_w / image_w), 0, screen_info.width - 1)
end

local function map_y(value, image_h, draw_h)
    return clamp((screen_info.height - draw_h) // 2 + math.floor(value * draw_h / image_h), 0, screen_info.height - 1)
end

local function draw_source_roi(detect_result, image_w, image_h, draw_w, draw_h)
    local source_w = detect_result.source_width or image_w
    local source_h = detect_result.source_height or image_h
    if source_w <= 0 or source_h <= 0 or source_w >= image_w and source_h >= image_h then
        return
    end

    local x = map_x(detect_result.source_x or 0, image_w, draw_w)
    local y = map_y(detect_result.source_y or 0, image_h, draw_h)
    local w = math.max(1, math.floor(source_w * draw_w / image_w))
    local h = math.max(1, math.floor(source_h * draw_h / image_h))
    screen:stroke_rect(x, y, clamp(w, 1, screen_info.width - x), clamp(h, 1, screen_info.height - y), { r = 64, g = 160, b = 255 })
end

local function draw_center_cross(cx, cy)
    local x = clamp(math.floor(cx + 0.5), 0, screen_info.width - 1)
    local y = clamp(math.floor(cy + 0.5), 0, screen_info.height - 1)
    local x0 = clamp(x - 6, 0, screen_info.width - 1)
    local y0 = clamp(y - 6, 0, screen_info.height - 1)
    screen:fill_rect(x0, y, math.min(13, screen_info.width - x0), 1, { r = 255, g = 255, b = 255 })
    screen:fill_rect(x, y0, 1, math.min(13, screen_info.height - y0), { r = 255, g = 255, b = 255 })
end

local function draw_color_box(detect_result, image_w, image_h)
    local draw_w, draw_h = fit_size(image_w, image_h, screen_info.width, screen_info.height)
    draw_source_roi(detect_result, image_w, image_h, draw_w, draw_h)

    if detect_result.detected ~= true then
        return
    end

    -- Match screen:image(..., mode = "contain") so the overlay box follows the preview pixels.
    local x1 = map_x(detect_result.left or detect_result.x or 0, image_w, draw_w)
    local y1 = map_y(detect_result.top or detect_result.y or 0, image_h, draw_h)
    local right = detect_result.right or ((detect_result.x or 0) + (detect_result.box_width or 1) - 1)
    local bottom = detect_result.bottom or ((detect_result.y or 0) + (detect_result.box_height or 1) - 1)
    local x2 = clamp((screen_info.width - draw_w) // 2 + math.floor((right + 1) * draw_w / image_w) - 1, 0, screen_info.width - 1)
    local y2 = clamp((screen_info.height - draw_h) // 2 + math.floor((bottom + 1) * draw_h / image_h) - 1, 0, screen_info.height - 1)
    local w = x2 - x1 + 1
    local h = y2 - y1 + 1

    if w <= 0 or h <= 0 then
        return
    end
    screen:stroke_rect(x1, y1, w, h, { r = 80, g = 255, b = 80 })
    if w > 4 and h > 4 then
        screen:stroke_rect(x1 + 1, y1 + 1, w - 2, h - 2, { r = 255, g = 255, b = 64 })
    end

    local cx = detect_result.cx and map_x(detect_result.cx, image_w, draw_w) or (x1 + w * 0.5)
    local cy = detect_result.cy and map_y(detect_result.cy, image_h, draw_h) or (y1 + h * 0.5)
    draw_center_cross(cx, cy)
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
    pcall(color_detect.release)
end

local function draw_result_overlay(frame_index, remaining_s, detect_result)
    local detected = detect_result.detected == true
    local pixels = detect_result.pixels or 0
    local score = detect_result.score or 0
    local status = detected and "FOUND" or "SEARCH"
    local bg = detected and { r = 24, g = 112, b = 48 } or { r = 24, g = 24, b = 24 }

    -- Draw an ASCII status bar after the camera preview so detection result is visible on screen.
    screen:fill_rect(0, 0, screen_info.width, 48, bg)
    screen:text(8, 6, string.format("color: %s", status), {
        color = "#ffffff",
        font_size = 16,
    })
    screen:text(8, 28, string.format("score=%.3f px=%d frame=%d left=%ds", score, pixels, frame_index, remaining_s), {
        color = "#ffffff",
        font_size = 12,
    })
end

local camera_devices = camera.list_devices()
if #camera_devices == 0 then
    print(TAG .. " SKIP: no capture video device available")
    return
end
local camera_path = camera_devices[1].path

local ok, err = pcall(display.open)
if not ok then
    print(TAG .. " SKIP: display.open failed: " .. tostring(err))
    return
end
screen = err
screen_info = screen:info()
display_started = true

ok, err = pcall(camera.open, camera_path, CAMERA_OPEN_OPTS)
if not ok then
    print(TAG .. " SKIP: camera.open failed: " .. tostring(err))
    cleanup()
    return
end
camera_started = true
print(string.format("%s using format=%s", TAG, camera.info().pixel_format))

local run_ok, run_err = xpcall(function()
    local stream = camera.info()
    local start_s = os.time()
    local deadline_s = start_s + RUN_SECONDS
    local frames = 0
    local detected_frames = 0
    local ticker = delay.periodic(FRAME_PERIOD_MS)

    print(string.format("%s start %ds stream=%dx%d format=%s", TAG, RUN_SECONDS, stream.width, stream.height, tostring(stream.pixel_format)))

    while os.time() < deadline_s do
        local now_s = os.time()
        local remaining_s = deadline_s - now_s
        local frame <close> = camera.get_frame(CAPTURE_TIMEOUT_MS)
        local rgb565 <close> = image.convert(frame, image.RGB565)
        local rgb_info = rgb565:info()
        local detect_result = color_detect.detect(rgb565, COLOR_OPTS)

        frames = frames + 1
        if detect_result.detected == true then
            detected_frames = detected_frames + 1
        end

        screen:begin({ clear = "#000000" })
        screen:image(0, 0, rgb565, {
            mode = "contain",
            width = screen_info.width,
            height = screen_info.height,
        })
        draw_color_box(detect_result, rgb_info.width, rgb_info.height)
        draw_result_overlay(frames, remaining_s, detect_result)
        screen:present()

        if frames == 1 or frames % 15 == 0 then
            print(string.format("%s frame=%d detected=%s score=%.3f pixels=%d box=%s,%s,%s,%s detected_frames=%d",
                TAG, frames, tostring(detect_result.detected), detect_result.score or 0, detect_result.pixels or 0,
                tostring(detect_result.left), tostring(detect_result.top), tostring(detect_result.right), tostring(detect_result.bottom), detected_frames))
        end

        ticker:wait()
    end

    screen:begin({ clear = "#000000" })
    center_text(0, 0, screen_info.width, screen_info.height, string.format("Color test done\nframes=%d detected=%d", frames, detected_frames), {
        color = "#ffffff",
        font_size = 20,
    })
    screen:present()

    print(string.format("%s PASS frames=%d detected_frames=%d", TAG, frames, detected_frames))
end, debug.traceback)

cleanup()

if not run_ok then
    error(run_err)
end
