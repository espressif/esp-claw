local audio = require("audio")

local input = assert(audio.open_input({ volume = 70 }))
local analyzer = assert(audio.analyzer({ input = input }))

local ok, err = xpcall(function()
    print("[audio_analyzer] input volume: " .. tostring(input:get_volume()))
    local level = analyzer:read_level({ duration_ms = 200 })
    print(string.format("[audio_analyzer] rms=%d peak=%d", level.rms, level.peak))
    local spectrum = analyzer:read_spectrum({ fft_size = 512, bands = 16 })
    print(string.format("[audio_analyzer] peak_freq=%.1fHz peak_db=%.1f bands=%d", spectrum.peak_freq_hz, spectrum.peak_db, spectrum.band_count))
end, debug.traceback)

pcall(function() analyzer:close() end)
pcall(function() input:close() end)
if not ok then
    error(err)
end
