local audio = require("audio")
local storage = require("storage")

local rec_path = storage.join_path(storage.get_root_dir(), "rec.aac")

local input = assert(audio.open_input({ volume = 70 }))
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

    output = assert(audio.open_output({ volume = 80 }))
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
