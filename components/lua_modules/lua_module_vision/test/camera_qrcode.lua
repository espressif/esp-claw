local board_manager = require("board_manager")
local camera = require("camera")
local delay = require("delay")
local display = require("display")
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
    local scale = math.min(1, display.width / image_w, display.height / image_h)
    local draw_w = math.floor(image_w * scale)
    local draw_h = math.floor(image_h * scale)

    -- Match display.draw_image(..., mode = "fit").
    for i = 1, result.count do
        local code = result[i]
        local x1 = clamp(math.floor(code.left * draw_w / image_w), 0, display.width - 1)
        local y1 = clamp(math.floor(code.top * draw_h / image_h), 0, display.height - 1)
        local x2 = clamp(math.floor((code.right + 1) * draw_w / image_w) - 1, 0, display.width - 1)
        local y2 = clamp(math.floor((code.bottom + 1) * draw_h / image_h) - 1, 0, display.height - 1)
        local width = x2 - x1 + 1
        local height = y2 - y1 + 1
        if width > 0 and height > 0 then
            display.draw_rect(x1, y1, width, height, { r = 80, g = 255, b = 80 })
            if width > 4 and height > 4 then
                display.draw_rect(x1 + 1, y1 + 1, width - 2, height - 2, { r = 255, g = 255, b = 64 })
            end
        end
    end
end

local function draw_status(frames, decoded, count)
    local found = count > 0
    display.fill_rect(0, 0, display.width, 48, found and { r = 24, g = 112, b = 48 } or { r = 24, g = 24, b = 24 })
    display.draw_text(8, 6, found and "QR: FOUND" or "QR: SEARCH", { color = "white", font_size = 16 })
    display.draw_text(8, 28, string.format("found=%d decoded=%d frame=%d", count, decoded, frames), { color = "white", font_size = 12 })
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
end

local panel_handle, io_handle, lcd_width, lcd_height, panel_if = board_manager.get_display_lcd_params("display_lcd")
if not panel_handle then
    print(TAG .. " SKIP: get_display_lcd_params failed: " .. tostring(io_handle))
    return
end

local devices = camera.list_devices()
if #devices == 0 then
    print(TAG .. " SKIP: no capture video device available")
    return
end

local ok, err = pcall(display.init, panel_handle, io_handle, lcd_width, lcd_height, panel_if)
if not ok then
    print(TAG .. " SKIP: display.init failed: " .. tostring(err))
    return
end
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

        display.begin_frame({ clear = true, color = "black" })
        display.draw_image(0, 0, rgb565, { mode = "fit", width = display.width, height = display.height })
        draw_codes(result, frame_info.width, frame_info.height)
        draw_status(frames, decoded, result.count)
        display.present()
        display.end_frame()

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
