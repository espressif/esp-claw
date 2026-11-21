local camera = require("camera")
local delay = require("delay")
local display = require("display")
local screen, screen_info
local espdet = require("espdet")
local image = require("image")
local storage = require("storage")

local TAG = "[espdet_face]"
local a = type(args) == "table" and args or {}
local root = storage.get_root_dir()
local run_seconds = tonumber(a.run_seconds) or 30
local frame_period_ms = tonumber(a.frame_period_ms) or 100
local capture_timeout_ms = tonumber(a.capture_timeout_ms) or 3000
local score_threshold = tonumber(a.score_threshold) or 0.5
-- The model is loaded from writable DATA storage by default.
local model_path = type(a.model_path) == "string" and a.model_path or storage.join_path(root, "models", "espdet_pico_224_224_face.espdl")
local camera_open_opts = { format = { "RGBP", "YUYV", "UYVY", "JPEG" }, width = 320, height = 240, nearest = true }

local display_started = false
local camera_started = false
local model_loaded = false

local function clamp(value, min_value, max_value)
    return math.max(min_value, math.min(value, max_value))
end

local function draw_faces(result, image_w, image_h)
    local scale = math.min(screen_info.width / image_w, screen_info.height / image_h)
    local draw_w = math.floor(image_w * scale)
    local draw_h = math.floor(image_h * scale)
    local ox, oy = (screen_info.width - draw_w) // 2, (screen_info.height - draw_h) // 2

    -- Match screen:image(..., mode = "contain").
    for i = 1, result.count do
        local face = result[i]
        local x1 = clamp(ox + math.floor(face.left * draw_w / image_w), 0, screen_info.width - 1)
        local y1 = clamp(oy + math.floor(face.top * draw_h / image_h), 0, screen_info.height - 1)
        local x2 = clamp(ox + math.floor((face.right + 1) * draw_w / image_w) - 1, 0, screen_info.width - 1)
        local y2 = clamp(oy + math.floor((face.bottom + 1) * draw_h / image_h) - 1, 0, screen_info.height - 1)
        local width = x2 - x1 + 1
        local height = y2 - y1 + 1
        if width > 0 and height > 0 then
            screen:stroke_rect(x1, y1, width, height, { r = 80, g = 255, b = 80 })
            if width > 4 and height > 4 then
                screen:stroke_rect(x1 + 1, y1 + 1, width - 2, height - 2, { r = 255, g = 255, b = 64 })
            end
        end

        local points = face.keypoint
        if points then
            for point = 1, #points - 1, 2 do
                local x = clamp(ox + math.floor(points[point] * draw_w / image_w), 0, screen_info.width - 1)
                local y = clamp(oy + math.floor(points[point + 1] * draw_h / image_h), 0, screen_info.height - 1)
                screen:stroke_circle(x, y, 2, { r = 255, g = 80, b = 80 })
            end
        end
    end
end

local function draw_status(frames, faces)
    local found = faces > 0
    screen:fill_rect(0, 0, screen_info.width, 48, found and { r = 24, g = 112, b = 48 } or { r = 24, g = 24, b = 24 })
    screen:text(8, 6, found and "FACE: FOUND" or "FACE: SEARCH", { color = "#ffffff", font_size = 16 })
    screen:text(8, 28, string.format("faces=%d frame=%d", faces, frames), { color = "#ffffff", font_size = 12 })
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
    if model_loaded then
        pcall(espdet.unload)
        model_loaded = false
    end
end

assert(storage.exists(model_path), "face model not found: " .. model_path)

local devices = camera.list_devices()
if #devices == 0 then
    print(TAG .. " SKIP: no capture video device available")
    return
end
local device = type(a.device) == "string" and a.device or devices[1].path

local ok, err = pcall(display.open)
if not ok then
    print(TAG .. " SKIP: display.open failed: " .. tostring(err))
    return
end
screen = err
screen_info = screen:info()
display_started = true

ok, err = pcall(camera.open, device, camera_open_opts)
if not ok then
    print(TAG .. " SKIP: camera.open failed: " .. tostring(err))
    cleanup()
    return
end
camera_started = true

local run_ok, run_err = xpcall(function()
    espdet.load(model_path, { score_threshold = score_threshold })
    model_loaded = true

    local stream = camera.info()
    local deadline_s = os.time() + run_seconds
    local frames = 0
    local ticker = delay.periodic(frame_period_ms)
    print(string.format("%s start stream=%dx%d format=%s", TAG, stream.width, stream.height, tostring(stream.pixel_format)))

    while os.time() < deadline_s do
        local frame <close> = camera.get_frame(capture_timeout_ms)
        local rgb565 <close> = image.convert(frame, image.RGB565)
        local frame_info = rgb565:info()
        local result = espdet.detect(rgb565, { score_threshold = score_threshold })
        frames = frames + 1

        screen:begin({ clear = "#000000" })
        screen:image(0, 0, rgb565, { mode = "contain", width = screen_info.width, height = screen_info.height })
        draw_faces(result, frame_info.width, frame_info.height)
        draw_status(frames, result.count)
        screen:present()

        if frames == 1 or frames % 10 == 0 then
            print(string.format("%s frame=%d faces=%d", TAG, frames, result.count))
            for i = 1, result.count do
                local face = result[i]
                print(string.format("%s face[%d] score=%.3f box=%d,%d,%d,%d", TAG, i, face.score,
                    face.left, face.top, face.right, face.bottom))
            end
        end
        ticker:wait()
    end
    print(string.format("%s PASS frames=%d", TAG, frames))
end, debug.traceback)

cleanup()
if not run_ok then
    error(run_err)
end
