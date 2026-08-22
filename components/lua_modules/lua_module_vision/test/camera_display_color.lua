local board_manager = require("board_manager")
local camera = require("camera")
local color_detect = require("color_detect")
local delay = require("delay")
local display = require("display")
local image = require("image")

local TAG = "[camera_display_color]"
local RUN_SECONDS = 30
local CAPTURE_TIMEOUT_MS = 3000
local FRAME_INTERVAL_MS = 30
-- Whatever the sensor exposes; image.convert covers any of these on output.
local CAMERA_OPEN_OPTS = { format = { "JPEG", "RGBP", "YUYV", "UYVY", "YU12" }, width = 320, height = 240, nearest = true, }
local COLOR_OPTS = {
    -- Default target is a saturated green object. Adjust HSV ranges for the object and lighting under test.
    h_min = 50,
    h_max = 88,
    s_min = 80,
    s_max = 255,
    v_min = 50,
    v_max = 255,
    min_pixels = 250,
    max_blob_pixels = 20000,
}

local display_started = false
local camera_started = false

local function fit_size(src_w, src_h, max_w, max_h)
    if src_w <= max_w and src_h <= max_h then
        return src_w, src_h
    end

    local ratio = math.min(max_w / src_w, max_h / src_h)
    local draw_w = math.floor(src_w * ratio)
    local draw_h = math.floor(src_h * ratio)
    if draw_w <= 0 then
        draw_w = 1
    end
    if draw_h <= 0 then
        draw_h = 1
    end
    if draw_w >= 8 then
        draw_w = draw_w - (draw_w % 8)
        if draw_w == 0 then
            draw_w = 8
        end
    end
    if draw_h >= 8 then
        draw_h = draw_h - (draw_h % 8)
        if draw_h == 0 then
            draw_h = 8
        end
    end
    return draw_w, draw_h
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
    return clamp(math.floor(value * draw_w / image_w), 0, display.width - 1)
end

local function map_y(value, image_h, draw_h)
    return clamp(math.floor(value * draw_h / image_h), 0, display.height - 1)
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
    display.draw_rect(x, y, clamp(w, 1, display.width - x), clamp(h, 1, display.height - y), { r = 64, g = 160, b = 255 })
end

local function draw_center_cross(cx, cy)
    local x = clamp(math.floor(cx + 0.5), 0, display.width - 1)
    local y = clamp(math.floor(cy + 0.5), 0, display.height - 1)
    local x0 = clamp(x - 6, 0, display.width - 1)
    local y0 = clamp(y - 6, 0, display.height - 1)
    display.fill_rect(x0, y, math.min(13, display.width - x0), 1, { r = 255, g = 255, b = 255 })
    display.fill_rect(x, y0, 1, math.min(13, display.height - y0), { r = 255, g = 255, b = 255 })
end

local function draw_color_box(detect_result, image_w, image_h)
    local draw_w, draw_h = fit_size(image_w, image_h, display.width, display.height)
    draw_source_roi(detect_result, image_w, image_h, draw_w, draw_h)

    if detect_result.detected ~= true then
        return
    end

    -- Match display.draw_image(..., mode = "fit") so the overlay box follows the preview pixels.
    local x1 = map_x(detect_result.left or detect_result.x or 0, image_w, draw_w)
    local y1 = map_y(detect_result.top or detect_result.y or 0, image_h, draw_h)
    local x2 = map_x((detect_result.right or ((detect_result.x or 0) + (detect_result.box_width or 1) - 1)) + 1, image_w, draw_w)
    local y2 = map_y((detect_result.bottom or ((detect_result.y or 0) + (detect_result.box_height or 1) - 1)) + 1, image_h, draw_h)
    local w = x2 - x1 + 1
    local h = y2 - y1 + 1

    if w <= 0 or h <= 0 then
        return
    end
    display.draw_rect(x1, y1, w, h, { r = 80, g = 255, b = 80 })
    if w > 4 and h > 4 then
        display.draw_rect(x1 + 1, y1 + 1, w - 2, h - 2, { r = 255, g = 255, b = 64 })
    end

    local cx = map_x(detect_result.cx or (x1 + w * 0.5), image_w, draw_w)
    local cy = map_y(detect_result.cy or (y1 + h * 0.5), image_h, draw_h)
    draw_center_cross(cx, cy)
end

local function cleanup()
    if display_started then
        pcall(display.end_frame)
        pcall(display.deinit)
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
    display.fill_rect(0, 0, display.width, 48, bg)
    display.draw_text(8, 6, string.format("color: %s", status), {
        color = "white",
        font_size = 16,
    })
    display.draw_text(8, 28, string.format("score=%.3f px=%d frame=%d left=%ds", score, pixels, frame_index, remaining_s), {
        color = "white",
        font_size = 12,
    })
end

local panel_handle, io_handle, lcd_width, lcd_height, panel_if = board_manager.get_display_lcd_params("display_lcd")
if not panel_handle then
    print(TAG .. " SKIP: get_display_lcd_params failed: " .. tostring(io_handle))
    return
end

local camera_paths, path_err = board_manager.get_camera_paths()
if not camera_paths then
    print(TAG .. " SKIP: get_camera_paths failed: " .. tostring(path_err))
    return
end

local ok, err = pcall(display.init, panel_handle, io_handle, lcd_width, lcd_height, panel_if)
if not ok then
    print(TAG .. " SKIP: display.init failed: " .. tostring(err))
    return
end
display_started = true

ok, err = pcall(camera.open, camera_paths.dev_path, CAMERA_OPEN_OPTS)
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

        display.begin_frame({ clear = true, color = "black" })
        display.draw_image(0, 0, rgb565, {
            mode = "fit",
            width = display.width,
            height = display.height,
        })
        draw_color_box(detect_result, rgb_info.width, rgb_info.height)
        draw_result_overlay(frames, remaining_s, detect_result)
        display.present()
        display.end_frame()

        if frames == 1 or frames % 15 == 0 then
            print(string.format("%s frame=%d detected=%s score=%.3f pixels=%d box=%s,%s,%s,%s detected_frames=%d",
                TAG, frames, tostring(detect_result.detected), detect_result.score or 0, detect_result.pixels or 0,
                tostring(detect_result.left), tostring(detect_result.top), tostring(detect_result.right), tostring(detect_result.bottom), detected_frames))
        end

        if FRAME_INTERVAL_MS > 0 then
            delay.delay_ms(FRAME_INTERVAL_MS)
        end
    end

    display.begin_frame({ clear = true, color = "black" })
    display.draw_text_aligned(0, 0, display.width, display.height, string.format("Color test done\nframes=%d detected=%d", frames, detected_frames), {
        color = "white",
        font_size = 20,
        align = "center",
        valign = "middle",
    })
    display.present()
    display.end_frame()

    print(string.format("%s PASS frames=%d detected_frames=%d", TAG, frames, detected_frames))
end, debug.traceback)

cleanup()

if not run_ok then
    error(run_err)
end
