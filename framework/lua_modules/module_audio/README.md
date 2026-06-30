# Lua Audio

Object-oriented Lua bindings for audio input, output, playback, recording, and
simple analysis. Board discovery stays in `board_manager`; this module only
wraps codec devices and audio processing helpers.

## How to call
- `local audio = require("audio")`
- Get codec handles and default formats from `board_manager.get_audio_codec_input_params(name)` or `board_manager.get_audio_codec_output_params(name)`
- `audio.new_output(desc)` opens an output codec device and returns an output object
- `audio.new_input(desc)` opens an input codec device and returns an input object
- `audio.player({ output = output })` creates a file, HTTP, or HTTPS player bound to one output object
- `audio.recorder({ input = input })` creates a WAV/AAC recorder bound to one input object
- `audio.analyzer({ input = input })` creates a level and spectrum analyzer bound to one input object
- Close player, recorder, and analyzer objects before closing their input or output device

## Device descriptors

`audio.new_output(desc)` and `audio.new_input(desc)` accept the compact table
returned by board manager:

```lua
local codec, rate, channels, bits = board_manager.get_audio_codec_output_params("audio_dac")
local output = audio.new_output({ codec, rate, channels, bits, volume = 80 })
```

Named fields are also accepted:

```lua
local output = audio.new_output({
    codec       = codec,
    sample_rate = 16000,
    channels    = 1,
    bits        = 16,
    volume      = 80,
})
```

Input and output devices both use `volume` as a percentage from 0 to 100:

```lua
local input = audio.new_input({ codec, rate, channels, bits, volume = 70 })
```

The device is opened immediately. `output:info()` and `input:info()` return the
actual device format after open, which may differ from the requested format on
devices such as UAC.

## Output objects
- `output:info()` returns `{ role, sample_rate, channels, bits, bytes_per_frame }`
- `output:set_volume(percent)` sets output volume from 0 to 100
- `output:get_volume()` returns the current output volume
- `output:set_mute(mute)` mutes or unmutes the output
- `output:write(pcm)` writes raw PCM in the output format
- `output:play_tone(freq_hz, duration_ms)` writes a generated sine tone
- `output:close()` closes the codec device

## Input objects
- `input:info()` returns `{ role, sample_rate, channels, bits, bytes_per_frame }`
- `input:set_volume(percent)` sets input capture volume from 0 to 100
- `input:get_volume()` returns the current input capture volume
- `input:read(bytes)` returns raw PCM from the input device
- `input:close()` closes the codec device

## Player

Create a player from an output object:

```lua
local player = audio.player({ output = output })
```

Supported calls:

- `player:play(path_or_uri [, opts])` starts playback
- `player:play(path_or_uri, { wait = true })` blocks until playback finishes
- `player:stop()` stops playback
- `player:pause()` pauses playback
- `player:resume()` resumes playback
- `player:poll()` returns `{ state, running, music_info = ... }`
- `player:close()` closes the player

Local paths are converted to `file://` URIs automatically. HTTP and HTTPS URIs
can be passed directly.

## Recorder

Create a recorder from an input object:

```lua
local recorder = audio.recorder({ input = input })
```

`recorder:record(path, opts)` requires `opts.duration_ms` and returns
`{ path, duration_ms, bytes, encoding, format }`.

```lua
local storage = require("storage")
local path = storage.join_path(storage.get_root_dir(), "rec.aac")
local info = recorder:record(path, {
    duration_ms = 3000,
    bitrate     = 64000,
})
print(info.path, info.bytes, info.encoding)
```

The output encoding is selected from the file extension. Unsupported extensions
return an error. Supported encodings:

- `.wav` writes PCM with a WAV header
- `.aac` writes AAC-LC with ADTS headers through `esp_audio_codec`

The recording output format defaults to the actual input format. It can be
overridden with `sample_rate`, `channels`, or `bits`. Input PCM is converted
automatically when the requested recording format differs from the device
format.

## Analyzer

Create an analyzer from an input object:

```lua
local analyzer = audio.analyzer({ input = input })
```

Supported calls:

- `analyzer:read_level(duration_ms)` returns RMS and peak level data
- `analyzer:read_spectrum(fft_size, bands)` returns spectrum bands and peak frequency data
- `analyzer:close()` closes the analyzer

## Example
```lua
local audio = require("audio")
local board_manager = require("board_manager")
local storage = require("storage")

local output_codec, output_rate, output_channels, output_bits =
    board_manager.get_audio_codec_output_params("audio_dac")
local output = assert(audio.new_output({ output_codec, output_rate, output_channels, output_bits, volume = 80 }))
local player = assert(audio.player({ output = output }))

local path = storage.join_path(storage.get_root_dir(), "static/test.mp3")
player:play(path, { wait = true })

player:close()
output:close()
```
