--[[
  image_resize.lua

  Live LCD test for image.resize(). Each captured camera frame is resized
  three ways and shown side-by-side so the operator can visually compare
  nearest vs bilinear, and confirm that GRAY8 resize works:

    left panel  : RGB565 nearest  scaled to fit panel
    right panel : RGB565 bilinear scaled to fit panel
    overlay     : per-frame timings for both filters and a small GRAY8 probe

  Hard-asserts the resized frame metadata (width/height/format/bytes) and that
  the resized frame outlives an explicit release of the source on a periodic
  spot check.
--]]

local board_manager = require("board_manager")
local camera = require("camera")
local delay = require("delay")
local display = require("display")
local image = require("image")
local system = require("system")

local TAG = "[image_resize]"
local RUN_SECONDS = 20
local CAPTURE_TIMEOUT_MS = 3000
local FRAME_INTERVAL_MS = 40
local CAMERA_OPEN_OPTS = { format = { "JPEG", "RGBP", "YUYV", "UYVY", "YU12" } }
local GRAY_PROBE_W = 64
local GRAY_PROBE_H = 64
local RELEASE_CHECK_EVERY_N = 10

local display_started = false
local camera_started = false

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

local function assert_true(value, message)
    if not value then
        error(message, 2)
    end
end

local function assert_info(view, expected_w, expected_h, expected_format, expected_bytes, label)
    local info = view:info()
    assert_true(info.valid == true, label .. ": view should be valid")
    assert_true(info.width == expected_w and info.height == expected_h,
        string.format("%s: size %dx%d expected %dx%d", label, info.width, info.height, expected_w, expected_h))
    assert_true(info.pixel_format == expected_format,
        string.format("%s: format=%s expected=%s", label, tostring(info.pixel_format), expected_format))
    assert_true(info.bytes == expected_bytes,
        string.format("%s: bytes=%d expected=%d", label, info.bytes, expected_bytes))
end

local panel_handle, io_handle, lcd_width, lcd_height, panel_if =
    board_manager.get_display_lcd_params("display_lcd")
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

local run_ok, run_err = xpcall(function()
    local overlay_h = 60
    local panel_w = display.width // 2
    local panel_h = math.max(32, display.height - overlay_h - 16)
    local target_w = math.max(32, panel_w - 4)
    local target_h = math.max(32, panel_h - 4)

    print(string.format("%s start %ds panels=%dx%d target=%dx%d",
        TAG, RUN_SECONDS, panel_w, panel_h, target_w, target_h))

    local start_ms = system.millis()
    local deadline_ms = start_ms + RUN_SECONDS * 1000
    local frames = 0
    local near_sum = 0
    local bilin_sum = 0
    local gray_sum = 0
    local release_checks = 0

    while system.millis() < deadline_ms do
        local now_ms = system.millis()
        local remaining_s = math.max(0, math.floor((deadline_ms - now_ms) / 1000))

        local frame <close> = camera.get_frame(CAPTURE_TIMEOUT_MS)
        assert_true(frame ~= nil, "camera.get_frame returned nil")

        local t0 = system.millis()
        local near = image.resize(frame, { width = target_w, height = target_h, filter = "nearest" })
        local t_near = system.millis() - t0
        assert_info(near, target_w, target_h, "RGBP", target_w * target_h * 2, "resize nearest")

        t0 = system.millis()
        local bilin = image.resize(frame, { width = target_w, height = target_h, filter = "bilinear" })
        local t_bilin = system.millis() - t0
        assert_info(bilin, target_w, target_h, "RGBP", target_w * target_h * 2, "resize bilinear")

        t0 = system.millis()
        local gray = image.resize(frame, { width = GRAY_PROBE_W, height = GRAY_PROBE_H,
                                           format = image.GRAY8, filter = "bilinear" })
        local t_gray = system.millis() - t0
        assert_info(gray, GRAY_PROBE_W, GRAY_PROBE_H, "GREY", GRAY_PROBE_W * GRAY_PROBE_H, "resize gray")

        frames = frames + 1
        near_sum = near_sum + t_near
        bilin_sum = bilin_sum + t_bilin
        gray_sum = gray_sum + t_gray

        -- Independent-store invariant: release the source frame early and
        -- confirm the resized views remain drawable.
        if frames % RELEASE_CHECK_EVERY_N == 0 then
            frame:release()
            assert_true(near:info().valid == true, "nearest resized view should survive source release")
            assert_true(bilin:info().valid == true, "bilinear resized view should survive source release")
            assert_true(gray:info().valid == true, "gray resized view should survive source release")
            release_checks = release_checks + 1
        end

        display.begin_frame({ clear = true, color = "black" })

        display.draw_image(2, overlay_h + 2, near, {
            mode = "fit", width = target_w, height = target_h,
        })
        display.draw_image(panel_w + 2, overlay_h + 2, bilin, {
            mode = "fit", width = target_w, height = target_h,
        })

        display.fill_rect(0, 0, display.width, overlay_h, { r = 24, g = 24, b = 24 })
        display.draw_text(6, 4, string.format("image.resize  frame=%d  left=%ds", frames, remaining_s),
            { color = "white", font_size = 14 })
        display.draw_text(6, 22, string.format("nearest=%dms  bilinear=%dms", t_near, t_bilin),
            { color = "white", font_size = 12 })
        display.draw_text(6, 38, string.format("gray%dx%d=%dms  release_checks=%d",
            GRAY_PROBE_W, GRAY_PROBE_H, t_gray, release_checks),
            { color = "white", font_size = 12 })

        local label_y = overlay_h + 2 + target_h + 2
        if label_y + 14 <= display.height then
            display.draw_text_aligned(0, label_y, panel_w, 14, "nearest",
                { color = "yellow", font_size = 12, align = "center" })
            display.draw_text_aligned(panel_w, label_y, panel_w, 14, "bilinear",
                { color = "yellow", font_size = 12, align = "center" })
        end

        display.present()
        display.end_frame()

        near:release()
        bilin:release()
        gray:release()

        if frames == 1 or frames % 10 == 0 then
            print(string.format("%s frame=%d near=%dms bilin=%dms gray=%dms",
                TAG, frames, t_near, t_bilin, t_gray))
        end

        if FRAME_INTERVAL_MS > 0 then
            delay.delay_ms(FRAME_INTERVAL_MS)
        end
    end

    assert_true(frames > 0, "no frames captured")

    local near_avg = near_sum / frames
    local bilin_avg = bilin_sum / frames
    local gray_avg = gray_sum / frames

    display.begin_frame({ clear = true, color = "black" })
    display.draw_text_aligned(0, 0, display.width, display.height,
        string.format("Resize PASS\nframes=%d\nnear=%.1fms\nbilin=%.1fms\ngray=%.1fms",
            frames, near_avg, bilin_avg, gray_avg),
        { color = "white", font_size = 18, align = "center", valign = "middle" })
    display.present()
    display.end_frame()

    print(string.format("%s PASS frames=%d near_avg=%.1fms bilin_avg=%.1fms gray_avg=%.1fms release_checks=%d",
        TAG, frames, near_avg, bilin_avg, gray_avg, release_checks))
end, debug.traceback)

cleanup()

if not run_ok then
    error(run_err)
end
