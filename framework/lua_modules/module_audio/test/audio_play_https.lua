local audio = require("audio")
local bm = require("board_manager")
local delay = require("delay")

local uri = "https://lhttp.qingting.fm/live/20071/64k.mp3"
local codec, rate, channels, bits = bm.get_audio_codec_output_params("audio_dac")
if not codec then
    error("get_audio_codec_output_params(audio_dac) failed: " .. tostring(rate))
end

local output = assert(audio.new_output({ codec, rate, channels, bits, volume = 70 }))
local player = assert(audio.player({ output = output }))

local ok, err = xpcall(function()
    local info = output:info()
    print(string.format("[audio_play_https] output=%dHz/%dch/%dbit uri=%s", info.sample_rate, info.channels, info.bits, uri))
    player:play(uri)
    delay.delay_ms(30000)
    player:stop()
    local state = player:poll()
    print("[audio_play_https] state=" .. tostring(state.state))
end, debug.traceback)

pcall(function() player:close() end)
pcall(function() output:close() end)
if not ok then
    error(err)
end
