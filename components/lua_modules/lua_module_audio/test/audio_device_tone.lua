local audio = require("audio")
local delay = require("delay")

local output = assert(audio.open_output({ volume = 100 }))

local ok, err = xpcall(function()
    local info = output:info()
    print(string.format("[audio_device_tone] output=%dHz/%dch/%dbit", info.sample_rate, info.channels, info.bits))
    output:set_volume(100)
    print("[audio_device_tone] current volume: " .. tostring(output:get_volume()))
    output:play_tone(523, 400)
    delay.delay_ms(400)
    output:play_tone(659, 400)
    delay.delay_ms(400)
    output:play_tone(784, 500)
    delay.delay_ms(500)
end, debug.traceback)

pcall(function() output:close() end)
if not ok then
    error(err)
end
