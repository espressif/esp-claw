local board_manager = require("board_manager")
local camera = require("camera")
local espdet = require("espdet")
local storage = require("storage")

local TAG = "[espdet_camera_test]"
local a = type(args) == "table" and args or {}
local ROOT = storage.get_root_dir()
local camera_paths, path_err = board_manager.get_camera_paths()
if not camera_paths then
    print(TAG .. " SKIP: get_camera_paths failed: " .. tostring(path_err))
    return
end

local DEVICE = type(a.device) == "string" and a.device or camera_paths.dev_path
local TIMEOUT_MS = 3000
local MODEL_PATH = type(a.model_path) == "string" and a.model_path or storage.join_path(ROOT, "models", "espdet_pico_224_224_cat.espdl")

espdet.load(MODEL_PATH, {
    score_threshold = 0.6,
})

camera.open(DEVICE)

do
    local frame <close> = camera.get_frame(TIMEOUT_MS)
    assert(frame ~= nil, "camera.get_frame returned nil")

    local result = espdet.detect(frame, {
        score_threshold = 0.6,
    })

    print("espdet count=" .. tostring(result.count))
    for i = 1, result.count do
        local det = result[i]
        print(string.format(
            "det[%d] score=%.3f category=%d box=(%d,%d)-(%d,%d)",
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

camera.close()
