local board_manager = require("board_manager")
local camera = require("camera")
local storage = require("storage")

local SAVE_PATH = storage.join_path(storage.get_root_dir(), "capture_demo.jpg")
local CAPTURE_TIMEOUT_MS = 3000

local function close_camera()
    local ok, err = pcall(camera.close)
    if not ok then
        print("[camera_capture_demo] WARN: close failed: " .. tostring(err))
    end
end

local camera_paths, path_err = board_manager.get_camera_paths()
if not camera_paths then
    print("[camera_capture_demo] ERROR: get_camera_paths failed: " .. tostring(path_err))
    return
end

print("[camera_capture_demo] camera dev_path: " .. tostring(camera_paths.dev_path))
if camera_paths.meta_path then
    print("[camera_capture_demo] camera meta_path: " .. tostring(camera_paths.meta_path))
end

local opened, open_err = pcall(camera.open, camera_paths.dev_path)
if not opened then
    print("[camera_capture_demo] ERROR: " .. tostring(open_err))
    return
end

local info_ok, info_or_err = pcall(camera.info)
if not info_ok then
    print("[camera_capture_demo] ERROR: " .. tostring(info_or_err))
    close_camera()
    return
end

print(string.format(
    "[camera_capture_demo] stream: %dx%d format=%s",
    info_or_err.width,
    info_or_err.height,
    tostring(info_or_err.pixel_format)
))

print(string.format(
    "[camera_capture_demo] capturing to %s (timeout=%d ms) ...",
    SAVE_PATH,
    CAPTURE_TIMEOUT_MS
))

local capture_ok, result_or_err = pcall(camera.capture, SAVE_PATH, CAPTURE_TIMEOUT_MS)
if not capture_ok then
    print("[camera_capture_demo] ERROR: " .. tostring(result_or_err))
    close_camera()
    return
end

print(string.format(
    "[camera_capture_demo] saved: path=%s bytes=%d frame=%dx%d format=%s timestamp_us=%d",
    result_or_err.path,
    result_or_err.bytes,
    result_or_err.width,
    result_or_err.height,
    tostring(result_or_err.pixel_format),
    result_or_err.timestamp_us
))

close_camera()
print("[camera_capture_demo] done")
