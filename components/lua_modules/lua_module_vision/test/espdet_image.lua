local espdet = require("espdet")
local image      = require("image")
local storage    = require("storage")

local TAG = "[espdet_image_test]"
local a = type(args) == "table" and args or {}
local ROOT = storage.get_root_dir()

local MODEL_PATH = a.model_path or storage.join_path(ROOT, "models", "espdet_pico_224_224_cat.espdl")
local IMAGE_PATH = a.image_path or storage.join_path(ROOT, "espdet_input.jpg")
local SCORE_THRESHOLD = tonumber(a.score_threshold) or 0.6
local NMS_THRESHOLD = tonumber(a.nms_threshold)

local function require_file(path, label)
    if not storage.exists(path) then
        error(string.format("%s missing %s: %s", TAG, label, path), 2)
    end
end

local function detect_options()
    local opts = {
        score_threshold = SCORE_THRESHOLD,
    }

    if NMS_THRESHOLD ~= nil then
        opts.nms_threshold = NMS_THRESHOLD
    end

    return opts
end

local function print_results(result)
    print(string.format("%s count=%d", TAG, result.count))
    for i = 1, result.count do
        local det = result[i]
        print(string.format(
            "%s det[%d] score=%.3f category=%d box=(%d,%d)-(%d,%d)",
            TAG,
            i,
            det.score,
            det.category,
            det.left,
            det.top,
            det.right,
            det.bottom
        ))
    end
end

require_file(MODEL_PATH, "model file")
require_file(IMAGE_PATH, "input image")

espdet.load(MODEL_PATH, detect_options())

do
    local source <close> = image.load_file(IMAGE_PATH)
    local source_info = source:info()

    print(string.format(
        "%s loaded %s: %dx%d %s bytes=%d",
        TAG,
        IMAGE_PATH,
        source_info.width,
        source_info.height,
        tostring(source_info.pixel_format),
        source_info.bytes
    ))

    local result = espdet.detect(source, detect_options())
    print_results(result)
end

espdet.unload()
print(TAG .. " PASS")
