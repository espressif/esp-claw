local camera = require("camera")
local delay = require("delay")
local display = require("display")
local screen, screen_info

local function center_text(x, y, w, h, text, options)
    local tw, th = screen:measure_text(text, options)
    screen:text(x + math.max(0, (w - tw) // 2), y + math.max(0, (h - th) // 2), text, options)
end
local image = require("image")
local motion = require("motion_detect")

local TAG = "[camera_display_motion]"
local RUN_SECONDS = 30
local CAPTURE_TIMEOUT_MS = 3000
local FRAME_PERIOD_MS = 30
-- Whatever the sensor exposes; image.convert covers any of these on output.
local CAMERA_OPEN_OPTS = { format = { "JPEG", "RGBP", "YUYV", "UYVY", "YU12" }, width = 320, height = 240, nearest = true, }
local MOTION_OPTS = {
    pixel_diff_threshold = 24,
    active_pixel_percent = 5,
    confirm_frames = 2,
    hold_frames = 3,
}

local display_started = false
local camera_started = false
local detector = motion.new(MOTION_OPTS)

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

local function draw_motion_box(detect_result, image_w, image_h)
    local box = detect_result.box
    if not box or image_w <= 0 or image_h <= 0 then
        return
    end

    -- Match screen:image(..., mode = "contain") so the overlay box follows the preview pixels.
    local draw_w, draw_h = fit_size(image_w, image_h, screen_info.width, screen_info.height)
    local ox, oy = (screen_info.width - draw_w) // 2, (screen_info.height - draw_h) // 2
    local x1 = clamp(ox + math.floor((box.left or box.x or 0) * draw_w / image_w), 0, screen_info.width - 1)
    local y1 = clamp(oy + math.floor((box.top or box.y or 0) * draw_h / image_h), 0, screen_info.height - 1)
    local x2 = clamp(ox + math.floor(((box.right or ((box.x or 0) + (box.width or 1) - 1)) + 1) * draw_w / image_w) - 1, 0, screen_info.width - 1)
    local y2 = clamp(oy + math.floor(((box.bottom or ((box.y or 0) + (box.height or 1) - 1)) + 1) * draw_h / image_h) - 1, 0, screen_info.height - 1)
    local w = x2 - x1 + 1
    local h = y2 - y1 + 1

    if w <= 0 or h <= 0 then
        return
    end
    screen:stroke_rect(x1, y1, w, h, { r = 255, g = 220, b = 40 })
    if w > 4 and h > 4 then
        screen:stroke_rect(x1 + 1, y1 + 1, w - 2, h - 2, { r = 255, g = 48, b = 48 })
    end
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

local function draw_result_overlay(frame_index, remaining_s, detect_result)
    local ready = detect_result.ready == true
    local score = detect_result.score or 0
    local moved = detect_result.motion == true
    local status = moved and "MOTION" or (ready and "STILL" or "WARMUP")
    local bg = { r = 24, g = 24, b = 24 }

    if moved then
        bg = { r = 160, g = 24, b = 24 }
    elseif ready then
        bg = { r = 24, g = 96, b = 48 }
    end

    -- Draw an ASCII status bar after the camera preview so detection result is visible on screen.
    screen:fill_rect(0, 0, screen_info.width, 48, bg)
    screen:text(8, 6, string.format("motion: %s", status), {
        color = "#ffffff",
        font_size = 16,
    })
    screen:text(8, 28, string.format("score=%.3f frame=%d left=%ds", score, frame_index, remaining_s), {
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
    local moved_frames = 0
    local ticker = delay.periodic(FRAME_PERIOD_MS)

    print(string.format("%s start %ds stream=%dx%d format=%s",
        TAG, RUN_SECONDS, stream.width, stream.height, tostring(stream.pixel_format)))

    while os.time() < deadline_s do
        local now_s = os.time()
        local remaining_s = deadline_s - now_s
        local frame <close> = camera.get_frame(CAPTURE_TIMEOUT_MS)
        local rgb565 <close> = image.convert(frame, image.RGB565)
        local rgb_info = rgb565:info()
        local detect_result = detector:detect(rgb565)

        frames = frames + 1
        if detect_result.motion == true then
            moved_frames = moved_frames + 1
        end

        screen:begin({ clear = "#000000" })
        screen:image(0, 0, rgb565, {
            mode = "contain",
            width = screen_info.width,
            height = screen_info.height,
        })
        draw_motion_box(detect_result, rgb_info.width, rgb_info.height)
        draw_result_overlay(frames, remaining_s, detect_result)
        screen:present()

        if frames == 1 or frames % 15 == 0 then
            print(string.format("%s frame=%d ready=%s motion=%s score=%.3f event=%s moved_frames=%d",
                TAG, frames, tostring(detect_result.ready), tostring(detect_result.motion),
                detect_result.score or 0, tostring(detect_result.event), moved_frames))
        end

        ticker:wait()
    end

    screen:begin({ clear = "#000000" })
    center_text(0, 0, screen_info.width, screen_info.height, string.format("Motion test done\nframes=%d moved=%d", frames, moved_frames), {
        color = "#ffffff",
        font_size = 20,
    })
    screen:present()

    print(string.format("%s PASS frames=%d moved_frames=%d", TAG, frames, moved_frames))
end, debug.traceback)

cleanup()
detector:close()

if not run_ok then
    error(run_err)
end
