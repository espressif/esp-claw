local audio = require("audio")
local storage = require("storage")

local path = storage.join_path(storage.get_root_dir(), "static/test.mp3")
local output = assert(audio.open_output({ volume = 90 }))
local player = assert(audio.player({ output = output }))

local ok, err = xpcall(function()
    local info = output:info()
    print(string.format("[audio_play_file] output=%dHz/%dch/%dbit path=%s", info.sample_rate, info.channels, info.bits, path))
    player:play(path, { wait = true })
    local state = player:poll()
    print("[audio_play_file] state=" .. tostring(state.state))
end, debug.traceback)

pcall(function() player:close() end)
pcall(function() output:close() end)
if not ok then
    error(err)
end
