local audio = require("audio")
local bm = require("board_manager")
local storage = require("storage")

local rec_path = storage.join_path(storage.get_root_dir(), "rec.aac")
local input_codec, input_rate, input_channels, input_bits = bm.get_audio_codec_input_params("audio_adc")
if not input_codec then
    error("get_audio_codec_input_params(audio_adc) failed: " .. tostring(input_rate))
end

local input = assert(audio.new_input({ input_codec, input_rate, input_channels, input_bits, volume = 70 }))
local recorder = assert(audio.recorder({ input = input }))
local output = nil
local player = nil

local ok, err = xpcall(function()
    local info = input:info()
    print(string.format("[audio_record_play_aac] input=%dHz/%dch/%dbit", info.sample_rate, info.channels, info.bits))
    local rec_info = recorder:record(rec_path, { duration_ms = 3000 })
    print(string.format("[audio_record_play_aac] path=%s bytes=%d duration=%d ms", rec_info.path, rec_info.bytes, rec_info.duration_ms))

    recorder:close()
    input:close()

    local output_codec, output_rate, output_channels, output_bits = bm.get_audio_codec_output_params("audio_dac")
    if not output_codec then
        error("get_audio_codec_output_params(audio_dac) failed: " .. tostring(output_rate))
    end
    output = assert(audio.new_output({ output_codec, output_rate, output_channels, output_bits, volume = 80 }))
    player = assert(audio.player({ output = output }))
    local out_info = output:info()
    print(string.format("[audio_record_play_aac] output=%dHz/%dch/%dbit", out_info.sample_rate, out_info.channels, out_info.bits))
    player:play(rec_path, { wait = true })
    local state = player:poll()
    print("[audio_record_play_aac] state=" .. tostring(state.state))
end, debug.traceback)

pcall(function() player:close() end)
pcall(function() output:close() end)
pcall(function() recorder:close() end)
pcall(function() input:close() end)
if not ok then
    error(err)
end
