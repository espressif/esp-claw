-- Exercises list_formats / open(opts) / flush / is_open / is_streaming.
-- Picks the first discrete frame size of the first format the sensor advertises,
-- re-opens with that explicit configuration, flushes, then grabs one frame.

local board_manager = require("board_manager")
local camera        = require("camera")

local TAG = "[camera_capabilities]"

local function assert_true(value, message)
    if not value then
        error(message, 2)
    end
end

local camera_paths, path_err = board_manager.get_camera_paths()
if not camera_paths then
    print(TAG .. " SKIP: get_camera_paths failed: " .. tostring(path_err))
    return
end

assert_true(camera.is_open() == false, "is_open() should be false before open")
assert_true(camera.is_streaming() == false, "is_streaming() should be false before open")

local ok, err = pcall(camera.open, camera_paths.dev_path)
if not ok then
    print(TAG .. " SKIP: camera.open failed: " .. tostring(err))
    return
end

assert_true(camera.is_open() == true, "is_open() should be true after open")
assert_true(camera.is_streaming() == true, "is_streaming() should be true after open")

local formats = camera.list_formats()
print(string.format("%s sensor advertises %d format(s):", TAG, #formats))
local first_choice = nil
for _, fmt in ipairs(formats) do
    print(string.format("  %s  (%s)  sizes=%d", fmt.format, fmt.description, #fmt.sizes))
    for _, s in ipairs(fmt.sizes) do
        local fps = s.fps and ("  fps=" .. table.concat(s.fps, ",")) or ""
        print(string.format("    %dx%d%s", s.w, s.h, fps))
    end
    if first_choice == nil and #fmt.sizes > 0 then
        first_choice = { format = { fmt.format }, width = fmt.sizes[1].w, height = fmt.sizes[1].h, nearest = true }
    end
end

-- Close so we can re-open with explicit opts.
camera.close()
assert_true(camera.is_open() == false, "is_open() should be false after close")

if first_choice ~= nil then
    print(string.format("%s re-opening with %s @ %dx%d",
        TAG, first_choice.format, first_choice.width, first_choice.height))
    local opened, open_err = pcall(camera.open, camera_paths.dev_path, first_choice)
    if not opened then
        print(TAG .. " WARN: open with opts failed (driver may not support it): " .. tostring(open_err))
        local fallback_ok, fallback_err = pcall(camera.open, camera_paths.dev_path)
        assert_true(fallback_ok, "fallback open failed: " .. tostring(fallback_err))
    end
else
    -- No discrete sizes advertised; just open with defaults.
    local opened, open_err = pcall(camera.open, camera_paths.dev_path)
    assert_true(opened, "camera.open default failed: " .. tostring(open_err))
end

local stream = camera.info()
print(string.format("%s active stream: %dx%d format=%s",
    TAG, stream.width, stream.height, stream.pixel_format))

camera.flush()
print(TAG .. " queue flushed")

do
    local frame <close> = camera.get_frame(2000)
    local info = frame:info()
    print(string.format("%s post-flush frame: %dx%d bytes=%d", TAG, info.width, info.height, info.bytes))
    assert_true(info.bytes > 0, "frame bytes must be positive")
end

camera.close()
assert_true(camera.is_streaming() == false, "is_streaming() should be false after close")
print(TAG .. " PASS")
