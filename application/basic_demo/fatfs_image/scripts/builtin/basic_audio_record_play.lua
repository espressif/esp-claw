local audio = require("audio")
local bm = require("board_manager")
local delay = require("delay")
local storage = require("storage")

local REC_PATH = storage.join_path(storage.get_root_dir(), "rec.wav")
local input_codec, input_rate, input_channels, input_bits, input_gain =
    bm.get_audio_codec_input_params("audio_adc")
if not input_codec then
    print("[audio_demo] ERROR: get_audio_codec_input_params(audio_adc) failed: " .. tostring(input_rate))
    return
end

local output_codec, output_rate, output_channels, output_bits =
    bm.get_audio_codec_output_params("audio_dac")
if not output_codec then
    print("[audio_demo] ERROR: get_audio_codec_output_params(audio_dac) failed: " .. tostring(output_rate))
    return
end

local input, in_err = audio.new_input(input_codec, input_rate, input_channels, input_bits)
if not input then
    print("[audio_demo] ERROR: new_input failed: " .. tostring(in_err))
    return
end

local output, out_err = audio.new_output(output_codec, output_rate, output_channels, output_bits)
if not output then
    print("[audio_demo] ERROR: new_output failed: " .. tostring(out_err))
    audio.close(input)
    return
end

print(string.format("[audio_demo] input=%dHz/%dch/%dbit output=%dHz/%dch/%dbit",
      input_rate, input_channels, input_bits,
      output_rate, output_channels, output_bits))

local function close_all()
    pcall(audio.close, output)
    pcall(audio.close, input)
end

local ok, err = xpcall(function()
    -- ── 1. volume and tone ───────────────────────────────────────────────────
    print("[audio_demo] setting volume to 100...")
    audio.set_volume(output, 100)
    local vol = audio.get_volume(output)
    print("[audio_demo] current volume: " .. tostring(vol))

    print("[audio_demo] playing a short tone sequence ...")
    audio.play_tone(output, 523, 180, 100)
    delay.delay_ms(100)
    audio.play_tone(output, 659, 180, 100)
    delay.delay_ms(100)
    audio.play_tone(output, 784, 240, 100)
    delay.delay_ms(100)
    print("[audio_demo] tone sequence done")

    -- ── 2. record ────────────────────────────────────────────────────────────
    print("[audio_demo] recording 3 seconds to " .. REC_PATH .. " ...")
    local info = audio.record_wav(input, REC_PATH, 3000)
    print(string.format("[audio_demo] recorded  path=%s  bytes=%d  duration=%d ms",
          info.path, info.bytes, info.duration_ms))

    delay.delay_ms(200)

    -- ── 3. play back the recording ───────────────────────────────────────────
    print("[audio_demo] playing back " .. REC_PATH .. " ...")
    audio.play_wav(output, REC_PATH)
    print("[audio_demo] playback done")
end, debug.traceback)

close_all()
if not ok then
    error(err)
end
