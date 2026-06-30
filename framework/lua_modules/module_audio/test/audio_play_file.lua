local audio = require("audio")
local bm = require("board_manager")
local storage = require("storage")

local path = storage.join_path(storage.get_root_dir(), "static/test.mp3")
local codec, rate, channels, bits = bm.get_audio_codec_output_params("audio_dac")
if not codec then
    error("get_audio_codec_output_params(audio_dac) failed: " .. tostring(rate))
end

local output = assert(audio.new_output({ codec, rate, channels, bits, volume = 90 }))
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
