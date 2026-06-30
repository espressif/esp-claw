-- --------------------------------------------------------------
-- Capture one JPEG still from the board camera.
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local board_manager = require("board_manager")
local camera = require("camera")
local image = require("image")
local storage = require("storage")

-- 2. Constants
local DEFAULT_FILENAME = "capture.jpg"
local DEFAULT_DIR = ""
local DEFAULT_TIMEOUT_MS = 3000
local DEFAULT_SKIP_FRAMES = 3

-- 3. Args
local function raw_arg(name, default)
  if type(args) == "table" and args[name] ~= nil then
    return args[name]
  end
  return default
end

local ARG_SCHEMA = {
  timeout_ms = arg_schema.int({ default = DEFAULT_TIMEOUT_MS, min = 0 }),
  skip_frames = arg_schema.int({ default = DEFAULT_SKIP_FRAMES, min = 0 }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)
ctx.filename = raw_arg("filename", DEFAULT_FILENAME)
ctx.dir = raw_arg("dir", DEFAULT_DIR)

-- 4. Validation
local function has_jpeg_suffix(path)
  local lower = string.lower(path)
  return string.sub(lower, -4) == ".jpg" or string.sub(lower, -5) == ".jpeg"
end

local function reject_path_part(name, value)
  if type(value) ~= "string" then
    error(name .. " must be a string")
  end
  if string.find(value, "%.%.", 1, false) then
    error(name .. " must not contain '..'")
  end
end

local function validate_filename(filename)
  reject_path_part("filename", filename)
  if filename == "" then
    error("filename must not be empty")
  end
  if string.find(filename, "/", 1, true) or string.find(filename, "\\", 1, true) then
    error("filename must not contain path separators")
  end
  if not has_jpeg_suffix(filename) then
    error("filename must end with .jpg or .jpeg")
  end
end

local function validate_dir(dir)
  reject_path_part("dir", dir)
  if string.find(dir, "/", 1, true) or string.find(dir, "\\", 1, true) then
    error("dir must be a single directory name under the storage root")
  end
end

local function build_save_path()
  validate_filename(ctx.filename)
  validate_dir(ctx.dir)

  local root = storage.get_root_dir()
  if ctx.dir == "" then
    return storage.join_path(root, ctx.filename)
  end

  local dir_path = storage.join_path(root, ctx.dir)
  if not storage.exists(dir_path) then
    storage.mkdir(dir_path)
  end
  return storage.join_path(dir_path, ctx.filename)
end

-- 5. Cleanup
local camera_opened = false

local function cleanup()
  if camera_opened then
    local ok, err = pcall(camera.close)
    if not ok then
      print("[take_picture] WARN: camera.close failed: " .. tostring(err))
    end
    camera_opened = false
  end
end

-- 6. Run
local function run()
  local camera_paths, path_err = board_manager.get_camera_paths()
  if not camera_paths then
    error("get_camera_paths failed: " .. tostring(path_err))
  end

  local save_path = build_save_path()

  local opened, open_err = pcall(camera.open, camera_paths.dev_path)
  if not opened then
    error(tostring(open_err))
  end
  camera_opened = true

  local info_ok, info_or_err = pcall(camera.info)
  if not info_ok then
    error(tostring(info_or_err))
  end

  print(string.format(
    "[take_picture] camera stream: %dx%d format=%s",
    info_or_err.width,
    info_or_err.height,
    tostring(info_or_err.pixel_format)
  ))

  camera.flush()

  -- Skip the first few frames after opening/flushing because some sensors produce overexposed warm-up frames.
  for i = 1, ctx.skip_frames do
    local warmup_frame <close> = camera.get_frame(ctx.timeout_ms)
    local warmup_info = warmup_frame:info()
    print(string.format(
      "[take_picture] skipped warm-up frame %d/%d: %dx%d format=%s timestamp_us=%d",
      i,
      ctx.skip_frames,
      warmup_info.width,
      warmup_info.height,
      tostring(warmup_info.pixel_format),
      warmup_info.timestamp_us
    ))
  end

  local frame <close> = camera.get_frame(ctx.timeout_ms)
  local frame_info = frame:info()
  image.save_file(save_path, frame)

  local saved_info, stat_err = storage.stat(save_path)
  if not saved_info then
    error("storage.stat failed after save: " .. tostring(stat_err))
  end

  print(string.format(
    "[take_picture] saved: path=%s bytes=%d frame=%dx%d format=%s timestamp_us=%d",
    save_path,
    saved_info.size,
    frame_info.width,
    frame_info.height,
    tostring(frame_info.pixel_format),
    frame_info.timestamp_us
  ))
end

-- 7. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
  print("[take_picture] ERROR: " .. tostring(err))
  error(err)
end
