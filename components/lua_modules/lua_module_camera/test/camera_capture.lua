local board_manager  = require("board_manager")
local camera         = require("camera")
local image          = require("image")
local storage        = require("storage")

local TAG = "[camera_capture_demo]"
local SAVE_PATH = storage.join_path(storage.get_root_dir(), "capture_demo.jpg")
local CAPTURE_TIMEOUT_MS = 3000

local function close_camera()
    local ok, err = pcall(camera.close)
    if not ok then
        print(TAG .. " WARN: close failed: " .. tostring(err))
    end
end

local camera_paths, path_err = board_manager.get_camera_paths()
if not camera_paths then
    print(TAG .. " ERROR: get_camera_paths failed: " .. tostring(path_err))
    return
end

print(TAG .. " camera dev_path: " .. tostring(camera_paths.dev_path))
if camera_paths.meta_path then
    print(TAG .. " camera meta_path: " .. tostring(camera_paths.meta_path))
end

local opened, open_err = pcall(camera.open, camera_paths.dev_path)
if not opened then
    print(TAG .. " ERROR: " .. tostring(open_err))
    return
end

local info_ok, info_or_err = pcall(camera.info)
if not info_ok then
    print(TAG .. " ERROR: " .. tostring(info_or_err))
    close_camera()
    return
end

print(string.format(
    "%s stream: %dx%d format=%s",
    TAG,
    info_or_err.width,
    info_or_err.height,
    tostring(info_or_err.pixel_format)
))

print(string.format(
    "%s capturing to %s (timeout=%d ms) ...",
    TAG,
    SAVE_PATH,
    CAPTURE_TIMEOUT_MS
))

local run_ok, run_err = xpcall(function()
    local frame <close> = camera.get_frame(CAPTURE_TIMEOUT_MS)
    local frame_info = frame:info()
    local jpeg_frame <close> = image.convert(frame, image.JPEG)
    local jpeg_info = jpeg_frame:info()
    image.save_file(SAVE_PATH, jpeg_frame)

    print(string.format(
        "%s saved: path=%s bytes=%d source=%dx%d source_format=%s timestamp_us=%d",
        TAG,
        SAVE_PATH,
        jpeg_info.bytes,
        frame_info.width,
        frame_info.height,
        tostring(frame_info.pixel_format),
        frame_info.timestamp_us
    ))
end, debug.traceback)

close_camera()

if not run_ok then
    print(TAG .. " ERROR: " .. tostring(run_err))
    return
end

print(TAG .. " done")
