local board_manager = require("board_manager")
local camera = require("camera")
local motion = require("motion_detect")

local TAG = "[motion_detect_test]"
local CAPTURE_TIMEOUT_MS = 3000
local MOTION_OPTS = {
    stride = 8,
    pixel_threshold = 0.2,
    moving_threshold = 0.02,
}

local function assert_true(value, message)
    if not value then
        error(message, 2)
    end
end

local function assert_type(value, expected_type, name)
    assert_true(type(value) == expected_type, string.format("%s expected %s, got %s", name, expected_type, type(value)))
end

local function close_camera()
    local ok, err = pcall(camera.close)
    if not ok then
        print(TAG .. " WARN: close failed: " .. tostring(err))
    end
end

local function get_frame_or_fail()
    local ok, frame_or_err = pcall(camera.get_frame, CAPTURE_TIMEOUT_MS)
    assert_true(ok, "camera.get_frame failed: " .. tostring(frame_or_err))
    return frame_or_err
end

local function release_frame(frame)
    local ok, err = pcall(function()
        frame:release()
    end)
    assert_true(ok, "frame:release failed: " .. tostring(err))
end

local camera_paths, path_err = board_manager.get_camera_paths()
if not camera_paths then
    print(TAG .. " SKIP: get_camera_paths failed: " .. tostring(path_err))
    return
end

print(TAG .. " camera dev_path: " .. tostring(camera_paths.dev_path))

local opened, open_err = pcall(camera.open, camera_paths.dev_path)
if not opened then
    print(TAG .. " SKIP: camera.open failed: " .. tostring(open_err))
    return
end

local info_ok, info_or_err = pcall(camera.info)
if not info_ok then
    close_camera()
    error("camera.info failed: " .. tostring(info_or_err))
end

print(string.format("%s stream: %dx%d format=%s", TAG, info_or_err.width, info_or_err.height, tostring(info_or_err.pixel_format)))

motion.reset()

local first_frame = get_frame_or_fail()
local first_result = motion.detect(first_frame, MOTION_OPTS)
release_frame(first_frame)

assert_type(first_result, "table", "first_result")
assert_true(first_result.has_previous == false, "first detect should seed previous frame")
assert_true(first_result.moving_points == nil, "first detect should not compare without previous frame")
assert_true(first_result.stride == MOTION_OPTS.stride, "first detect stride mismatch")
assert_true(first_result.pixel_threshold == MOTION_OPTS.pixel_threshold, "first detect pixel_threshold mismatch")
print(TAG .. " first detect seeded previous frame")

local second_frame = get_frame_or_fail()
local second_result = motion.detect(second_frame, MOTION_OPTS)
release_frame(second_frame)

assert_type(second_result, "table", "second_result")
assert_true(second_result.has_previous == true, "second detect should compare with previous frame")
assert_type(second_result.moving_points, "number", "second_result.moving_points")
assert_true(second_result.moving_points >= 0, "moving_points must be non-negative")
assert_type(second_result.sample_points, "number", "second_result.sample_points")
assert_true(second_result.sample_points > 0, "sample_points must be positive")
assert_type(second_result.moving_ratio, "number", "second_result.moving_ratio")
assert_true(second_result.moving_ratio >= 0 and second_result.moving_ratio <= 1, "moving_ratio must be in [0, 1]")
assert_true(second_result.stride == MOTION_OPTS.stride, "second detect stride mismatch")
assert_true(second_result.pixel_threshold == MOTION_OPTS.pixel_threshold, "second detect pixel_threshold mismatch")
assert_true(second_result.moving_threshold == MOTION_OPTS.moving_threshold, "second detect moving_threshold mismatch")
assert_type(second_result.moved, "boolean", "second_result.moved")
print(string.format("%s second detect: moving_points=%d sample_points=%d moving_ratio=%.4f moved=%s",
    TAG, second_result.moving_points, second_result.sample_points, second_result.moving_ratio, tostring(second_result.moved)))

motion.reset()

local reset_frame = get_frame_or_fail()
local reset_result = motion.detect(reset_frame, MOTION_OPTS)
release_frame(reset_frame)

assert_type(reset_result, "table", "reset_result")
assert_true(reset_result.has_previous == false, "detect after reset should seed previous frame again")
assert_true(reset_result.moving_points == nil, "detect after reset should not compare without previous frame")
print(TAG .. " reset behavior verified")

close_camera()
print(TAG .. " PASS")
