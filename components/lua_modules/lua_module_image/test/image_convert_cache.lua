--[[
  image_convert_cache.lua

  Exercises lua_module_image conversion + shared-store cache behaviour using
  live camera frames and the LCD. Each frame:

    1. Converts to RGB565 twice: first call is the cache-miss path, second
       call must hit the shared-store cache and complete significantly faster.
    2. Same for GRAY8.
    3. Periodically re-encodes to JPEG and verifies that converted views stay
       alive after the source frame is released (README contract).

  Renders the cached RGB565 preview on screen with a status bar showing the
  miss / hit timings so the operator can see the cache working in real time.
--]]

local board_manager = require("board_manager")
local camera = require("camera")
local delay = require("delay")
local display = require("display")
local image = require("image")
local system = require("system")

local TAG = "[image_convert_cache]"
local RUN_SECONDS = 20
local CAPTURE_TIMEOUT_MS = 3000
local FRAME_INTERVAL_MS = 30
-- JPEG encode is heavy; only run it every N frames so the preview keeps moving.
local JPEG_EVERY_N_FRAMES = 10
-- Verify "source release does not invalidate cached view" every N frames.
local RELEASE_CHECK_EVERY_N_FRAMES = 5
local CAMERA_OPEN_OPTS = { format = { "JPEG", "RGBP", "YUYV", "UYVY", "YU12" }, width = 320, height = 240, nearest = true, }
-- Hit must be at most this ratio of miss; allows for jitter while still
-- catching a regression that disables the cache entirely.
local CACHE_HIT_MAX_RATIO = 0.5

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

-- Returns elapsed milliseconds and the produced frame view.
local function timed_convert(frame, fmt)
    local t0 = system.millis()
    local view = image.convert(frame, fmt)
    local elapsed = system.millis() - t0
    return elapsed, view
end

local function assert_format(view, expected_format, expected_bytes_at_least, label)
    local info = view:info()
    assert_true(type(info) == "table", label .. ": info() should return a table")
    assert_true(info.valid == true, label .. ": view should be valid")
    assert_true(info.pixel_format == expected_format,
        string.format("%s: pixel_format=%s expected=%s", label, tostring(info.pixel_format), expected_format))
    assert_true(info.bytes >= expected_bytes_at_least,
        string.format("%s: bytes=%d expected>=%d", label, info.bytes, expected_bytes_at_least))
    return info
end

-- Hard cache assertion. miss==0 is possible on tiny frames; treat that as a
-- pass (we cannot prove the cache hit faster than zero). Otherwise hit must
-- be at most CACHE_HIT_MAX_RATIO of miss.
local function assert_cache_hit(miss_ms, hit_ms, label)
    if miss_ms <= 0 then
        return
    end
    local threshold = math.max(1, math.floor(miss_ms * CACHE_HIT_MAX_RATIO))
    assert_true(hit_ms <= threshold,
        string.format("%s: cache hit too slow: miss=%dms hit=%dms threshold=%dms",
            label, miss_ms, hit_ms, threshold))
end

local function draw_status_overlay(stats)
    local bg = stats.last_error and { r = 160, g = 24, b = 24 } or { r = 24, g = 24, b = 24 }
    display.fill_rect(0, 0, display.width, 60, bg)

    local line1 = string.format("frame=%d  left=%ds  fps~%.1f",
        stats.frame_index, stats.remaining_s, stats.fps_est)
    display.draw_text(6, 4, line1, { color = "white", font_size = 14 })

    local line2 = string.format("RGB565 miss=%dms hit=%dms",
        stats.last_rgb_miss, stats.last_rgb_hit)
    display.draw_text(6, 22, line2, { color = "white", font_size = 12 })

    local line3 = string.format("GRAY8  miss=%dms hit=%dms  JPEG=%s",
        stats.last_gray_miss, stats.last_gray_hit,
        stats.last_jpeg_ms >= 0 and (stats.last_jpeg_ms .. "ms") or "-")
    display.draw_text(6, 38, line3, { color = "white", font_size = 12 })

    if stats.last_error then
        display.draw_text(6, 52, "FAIL: " .. stats.last_error, { color = "yellow", font_size = 12 })
    end
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
    local stream = camera.info()
    print(string.format("%s start %ds stream=%dx%d format=%s",
        TAG, RUN_SECONDS, stream.width, stream.height, tostring(stream.pixel_format)))

    local start_ms = system.millis()
    local deadline_ms = start_ms + RUN_SECONDS * 1000
    local frames = 0
    local rgb_speedup_sum = 0
    local rgb_speedup_count = 0
    local gray_speedup_sum = 0
    local gray_speedup_count = 0
    local jpeg_runs = 0
    local release_checks = 0

    local stats = {
        frame_index = 0,
        remaining_s = RUN_SECONDS,
        fps_est = 0,
        last_rgb_miss = 0,
        last_rgb_hit = 0,
        last_gray_miss = 0,
        last_gray_hit = 0,
        last_jpeg_ms = -1,
        last_error = nil,
    }

    while system.millis() < deadline_ms do
        local now_ms = system.millis()
        stats.remaining_s = math.max(0, math.floor((deadline_ms - now_ms) / 1000))

        local frame <close> = camera.get_frame(CAPTURE_TIMEOUT_MS)
        assert_true(frame ~= nil, "camera.get_frame returned nil")
        local src_info = frame:info()
        local pixel_count = src_info.width * src_info.height

        -- Conversion + cache: RGB565
        local rgb_miss_ms, rgb_miss_view = timed_convert(frame, image.RGB565)
        assert_format(rgb_miss_view, "RGBP", pixel_count * 2, "RGB565 miss")
        local rgb_hit_ms, rgb_hit_view = timed_convert(frame, image.RGB565)
        assert_format(rgb_hit_view, "RGBP", pixel_count * 2, "RGB565 hit")
        assert_cache_hit(rgb_miss_ms, rgb_hit_ms, "RGB565")

        -- Conversion + cache: GRAY8
        local gray_miss_ms, gray_miss_view = timed_convert(frame, image.GRAY8)
        assert_format(gray_miss_view, "GREY", pixel_count, "GRAY8 miss")
        local gray_hit_ms, gray_hit_view = timed_convert(frame, image.GRAY8)
        assert_format(gray_hit_view, "GREY", pixel_count, "GRAY8 hit")
        assert_cache_hit(gray_miss_ms, gray_hit_ms, "GRAY8")

        frames = frames + 1
        if rgb_miss_ms > 0 then
            rgb_speedup_sum = rgb_speedup_sum + (rgb_miss_ms / math.max(1, rgb_hit_ms))
            rgb_speedup_count = rgb_speedup_count + 1
        end
        if gray_miss_ms > 0 then
            gray_speedup_sum = gray_speedup_sum + (gray_miss_ms / math.max(1, gray_hit_ms))
            gray_speedup_count = gray_speedup_count + 1
        end

        -- Periodic JPEG re-encode
        local jpeg_ms = -1
        if frames % JPEG_EVERY_N_FRAMES == 0 then
            local jpeg_view
            jpeg_ms, jpeg_view = timed_convert(frame, image.JPEG)
            local jpeg_info = jpeg_view:info()
            assert_true(jpeg_info.pixel_format == "JPEG", "JPEG view format mismatch: " .. tostring(jpeg_info.pixel_format))
            assert_true(jpeg_info.bytes > 0, "JPEG view bytes should be > 0")
            jpeg_view:release()
            jpeg_runs = jpeg_runs + 1
        end

        -- Periodically verify converted views outlive the source frame.
        if frames % RELEASE_CHECK_EVERY_N_FRAMES == 0 then
            local survivor = image.convert(frame, image.RGB565)
            frame:release()
            local survivor_info = survivor:info()
            assert_true(survivor_info.valid == true, "cached RGB565 view should survive source release")
            assert_true(survivor_info.bytes >= pixel_count * 2, "cached RGB565 view bytes shrank after source release")
            survivor:release()
            release_checks = release_checks + 1
        end

        -- Render the preview using the cached (hit) RGB565 view.
        local elapsed_s = math.max(1, (system.millis() - start_ms) // 1000)
        stats.frame_index = frames
        stats.fps_est = frames / elapsed_s
        stats.last_rgb_miss = rgb_miss_ms
        stats.last_rgb_hit = rgb_hit_ms
        stats.last_gray_miss = gray_miss_ms
        stats.last_gray_hit = gray_hit_ms
        stats.last_jpeg_ms = jpeg_ms

        display.begin_frame({ clear = true, color = "black" })
        display.draw_image(0, 0, rgb_hit_view, {
            mode = "fit",
            width = display.width,
            height = display.height,
        })
        draw_status_overlay(stats)
        display.present()
        display.end_frame()

        -- Release per-frame views so the camera buffer can be returned.
        rgb_miss_view:release()
        rgb_hit_view:release()
        gray_miss_view:release()
        gray_hit_view:release()

        if frames == 1 or frames % 10 == 0 then
            print(string.format(
                "%s frame=%d rgb miss/hit=%d/%dms gray miss/hit=%d/%dms jpeg=%s",
                TAG, frames, rgb_miss_ms, rgb_hit_ms, gray_miss_ms, gray_hit_ms,
                jpeg_ms >= 0 and (jpeg_ms .. "ms") or "-"))
        end

        if FRAME_INTERVAL_MS > 0 then
            delay.delay_ms(FRAME_INTERVAL_MS)
        end
    end

    assert_true(frames > 0, "no frames captured")

    local rgb_speedup_avg = rgb_speedup_count > 0 and (rgb_speedup_sum / rgb_speedup_count) or 0
    local gray_speedup_avg = gray_speedup_count > 0 and (gray_speedup_sum / gray_speedup_count) or 0

    display.begin_frame({ clear = true, color = "black" })
    display.draw_text_aligned(0, 0, display.width, display.height,
        string.format("Convert+cache PASS\nframes=%d\nRGB565 x%.1f  GRAY8 x%.1f\njpeg=%d release=%d",
            frames, rgb_speedup_avg, gray_speedup_avg, jpeg_runs, release_checks),
        { color = "white", font_size = 18, align = "center", valign = "middle" })
    display.present()
    display.end_frame()

    print(string.format(
        "%s PASS frames=%d rgb_speedup_avg=%.2f gray_speedup_avg=%.2f jpeg_runs=%d release_checks=%d",
        TAG, frames, rgb_speedup_avg, gray_speedup_avg, jpeg_runs, release_checks))
end, debug.traceback)

cleanup()

if not run_ok then
    error(run_err)
end
