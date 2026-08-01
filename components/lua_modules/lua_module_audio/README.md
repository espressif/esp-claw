# Lua Audio

Object-oriented Lua bindings for audio input, output, playback, recording, and
simple analysis. This module routes every call through the process-wide
`audio_mixer` (output) and `audio_capture` (input) compositors so Lua scripts
can share the codec with the C system. Formats and codec discovery live inside
the compositors — `board_manager` does not expose codec params anymore.

## Runtime model

- The codec DAC is owned by a single `audio_mixer` started by `app_claw`.
  Internally it holds two tracks: SYSTEM (C prompts / TTS) and APP (Lua
  producers). When SYSTEM has non-silence the APP track is ducked
  automatically; when SYSTEM has been silent for the configured release the
  APP track fades back to full gain. Lua callers never see these tracks by
  name — the module always binds to APP.
- The codec ADC is owned by a single `audio_capture` hub with two internal
  subscriber slots: SYSTEM (wake-word / ASR) and APP (Lua recorders /
  analyzers). Each subscriber reads from its own ring buffer, so a slow
  reader only drops its own frames.
- Because the module is hard-bound to the APP slot, at most one Lua output
  and one Lua input can be open at a time; opening a second one returns
  `nil, "audio output: app track already open"` or
  `nil, "audio input: app subscriber already open"`.
- When the board has no codec (or the compositor could not start) opening a
  device returns `nil, "audio codec not available"`.

## How to call
- `local audio = require("audio")`
- `audio.open_output([opts])` opens the shared output on the mixer
- `audio.open_input([opts])` opens the shared input on the capture hub
- `audio.player({ output = output })` creates a file, HTTP, or HTTPS player bound to the output
- `audio.recorder({ input = input })` creates a WAV/AAC recorder bound to the input
- `audio.analyzer({ input = input })` creates a level and spectrum analyzer bound to the input
- Close player, recorder, and analyzer objects before closing their input or output device

## Opening output and input

`audio.open_output` and `audio.open_input` accept either no argument or an
opts table `{ volume = 80, sample_rate = ..., channels = ..., bits = ... }`.
No role field is required — every Lua caller lands on the APP slot.

```lua
local output = assert(audio.open_output())                 -- defaults
local output = assert(audio.open_output({ volume = 80 }))  -- with options
```

For the output object the format fields on the opts table are ignored — the
mixer's negotiated PCM format wins. `output:info()` returns the mixer's format
(what producers must write). For the input object the format fields are passed
through to `audio_capture_open_subscriber`, which sets up per-subscriber format
conversion when they differ from the hub's internal format. Missing fields fall
back to the hub defaults.

```lua
local input = assert(audio.open_input({ volume = 70 }))
```

## Output objects
- `output:info()` returns `{ role, sample_rate, channels, bits, bytes_per_frame }` from the mixer track
- `output:set_volume(percent)` sets the physical codec DAC volume (0..100) through the mixer; per-track software gain remains reserved for the mixer's duck policy
- `output:get_volume()` returns the current codec DAC volume as reported by the mixer
- `output:set_mute(mute)` no-op in v1; mute is managed by the mixer
- `output:write(pcm)` writes raw PCM in the mixer's frame format
- `output:play_tone(freq_hz, duration_ms)` writes a generated sine tone through the mixer
- `output:close()` closes the mixer APP track

`output:write` splits large writes into ring-sized chunks with a short pacing
delay so the mixer task can drain between chunks. Writes never block for long,
but if a producer pushes faster than the mixer consumes, the underlying ring
drops the oldest samples silently (matching `audio_mixer` semantics).

## Input objects
- `input:info()` returns `{ role, sample_rate, channels, bits, bytes_per_frame }` from the capture subscriber
- `input:set_volume(percent)` records the requested value; gain is managed by the capture hub in v1 (no-op)
- `input:get_volume()` returns the last requested value
- `input:read(bytes)` returns raw PCM from the capture APP subscriber
- `input:close()` closes the capture APP subscriber

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

The recording output format defaults to the actual input subscriber format. It
can be overridden with `sample_rate`, `channels`, or `bits`. Input PCM is
converted automatically when the requested recording format differs from the
subscriber format.

## Analyzer

Create an analyzer from an input object:

```lua
local analyzer = audio.analyzer({ input = input })
```

Supported calls:

- `analyzer:read_level(duration_ms)` returns RMS and peak level data
- `analyzer:read_spectrum(fft_size, bands)` returns spectrum bands and peak frequency data
- `analyzer:close()` closes the analyzer

Recorder and analyzer share the single APP subscriber on the capture hub. Open
one at a time: if a recorder is active, opening an analyzer on the same input
returns `nil, "audio input: app subscriber already open"`, and vice versa.
Close the first object (or its underlying `input`) before opening the other.

## Example
```lua
local audio = require("audio")
local storage = require("storage")

local output = assert(audio.open_output({ volume = 80 }))
local player = assert(audio.player({ output = output }))

local path = storage.join_path(storage.get_root_dir(), "static/test.mp3")
player:play(path, { wait = true })

player:close()
output:close()
```
