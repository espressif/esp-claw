local board_manager = require("board_manager")
local camera = require("camera")
local motion = require("motion_detect")

local TAG = "[motion_detect_test]"
local CAPTURE_TIMEOUT_MS = 3000
local MOTION_OPTS = {
    pixel_diff_threshold = 24,
    active_pixel_percent = 5,
    confirm_frames = 2,
    hold_frames = 3,
}

local function assert_true(value, message)
    if not value then
        error(message, 2)
    end
end

local function assert_type(value, expected_type, name)
    assert_true(type(value) == expected_type,
        string.format("%s expected %s, got %s", name, expected_type, type(value)))
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

local camera_paths, path_err = board_manager.get_camera_paths()
if not camera_paths then
    print(TAG .. " SKIP: get_camera_paths failed: " .. tostring(path_err))
    return
end

local opened, open_err = pcall(camera.open, camera_paths.dev_path)
if not opened then
    print(TAG .. " SKIP: camera.open failed: " .. tostring(open_err))
    return
end

local detector = motion.new(MOTION_OPTS)
local run_ok, run_err = xpcall(function()
    local first_result
    do
        local first_frame <close> = get_frame_or_fail()
        first_result = detector:detect(first_frame)
    end

    assert_type(first_result, "table", "first_result")
    assert_true(first_result.ready == false, "first detect should seed the previous frame")
    assert_true(first_result.motion == false, "first detect must not report motion")
    assert_true(first_result.event == "none", "first detect event must be none")
    assert_type(first_result.score, "number", "first_result.score")

    local second_result
    do
        local second_frame <close> = get_frame_or_fail()
        second_result = detector:detect(second_frame)
    end
    assert_true(second_result.ready == true, "second detect should compare with the previous frame")
    assert_type(second_result.motion, "boolean", "second_result.motion")
    assert_type(second_result.score, "number", "second_result.score")
    assert_true(second_result.score >= 0 and second_result.score <= 1, "second_result.score must be in [0, 1]")
    assert_true(second_result.event == "none" or second_result.event == "started" or second_result.event == "stopped",
        "unexpected motion event")

    detector:reset()
    local reset_result
    do
        local reset_frame <close> = get_frame_or_fail()
        reset_result = detector:detect(reset_frame)
    end
    assert_true(reset_result.ready == false, "detect after reset should seed again")
    print(string.format("%s PASS motion=%s score=%.3f", TAG,
        tostring(second_result.motion), second_result.score))
end, debug.traceback)

detector:close()
close_camera()
if not run_ok then
    error(run_err)
end
