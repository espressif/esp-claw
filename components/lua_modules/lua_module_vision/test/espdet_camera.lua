local camera = require("camera")
local espdet = require("espdet")
local storage = require("storage")

local DEVICE = "/dev/video0"
local TIMEOUT_MS = 3000
local MODEL_PATH = storage.join_path(storage.get_root_dir(), "models/espdet_pico_224_224_cat.espdl")

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
