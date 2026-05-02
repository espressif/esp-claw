# Lua Audio

This skill describes how to correctly use audio when writing Lua scripts.

## How to call
- Import it with `local audio = require("audio")`
- Call `audio.new_input(codec_dev_handle, sample_rate, channels, bits_per_sample [, gain_db])` to create an input handle
- Call `audio.new_output(codec_dev_handle, sample_rate, channels, bits_per_sample [, volume])` to create an output handle
- Call `audio.play_tone(output_handle, freq_hz, duration_ms [, volume_pct [, wait_done]])` to generate and play a sine tone on a 16-bit PCM output handle
- Call `audio.play_wav(output_handle, path)` to play a WAV file under `/fatfs/data/`
- Call `audio.record_wav(input_handle, path, duration_ms)` to record audio to a WAV file under `/fatfs/data/`
- Call `audio.loopback(input_handle, output_handle [, duration_ms])` to route input to output for monitoring
- Call `audio.set_volume(output_handle, pct)`, `audio.get_volume(output_handle)`, `audio.set_mute(output_handle, enabled)`, or `audio.set_gain(input_handle, db)` to adjust levels
- Call `audio.mic_read_level(input_handle [, duration_ms])` to read microphone level statistics such as `rms` and `peak`
- Call `audio.read_spectrum(input_handle [, fft_size] [, band_count])` to capture one frame of 16-bit PCM audio and return FFT spectrum data such as `bands`, `peak_freq_hz`, `peak_db`, and `rms`
- Call `audio.close(handle)` when a created handle is no longer needed

## Example
```lua
local audio = require("audio")
local bm = require("board_manager")
local storage = require("storage")

local output_codec, rate, channels, bits =
    bm.get_audio_codec_output_params("audio_dac")
local input_codec =
    bm.get_audio_codec_input("audio_adc")
local output = audio.new_output(output_codec, rate, channels, bits)
local input = audio.new_input(input_codec, rate, channels, bits)
local wav_path = storage.join_path(storage.get_root_dir(), "test.wav")

audio.set_volume(output, 60)

audio.play_tone(output, 880, 200, 35, true)

local rec = audio.record_wav(input, wav_path, 1000)
print("recorded:", rec.path, rec.duration_ms, rec.bytes)

audio.play_wav(output, wav_path)

local level = audio.mic_read_level(input, 100)
print("mic rms:", level.rms, "peak:", level.peak)

local spectrum = audio.read_spectrum(input, 512, 16)
print("peak freq:", spectrum.peak_freq_hz, "peak db:", spectrum.peak_db, "rms:", spectrum.rms)

for i, band in ipairs(spectrum.bands) do
    print("band", i, band)
end

audio.close(input)
audio.close(output)
```
