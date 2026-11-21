# Lua Audio

The `audio` module provides Lua APIs for PCM input and output, media playback,
recording, polyphonic sample playback, and basic audio analysis.

```lua
local audio = require("audio")
```

## Resource and error handling

- At most one input device and one output device can be open at a time.
- Close players before their output device, and close recorders and analyzers
  before their input device. Closing a device that is still in use returns
  `nil, "audio device: busy"`.
- Operations that fail at runtime return `nil, error_message`. Invalid arguments
  may raise a Lua error; validate user-supplied values before calling.
- `close()` is safe to call more than once and returns `true`.

## Open devices

### `audio.open_output([opts])`

Opens an output device and returns `output`, or `nil, error_message`.

`opts` is optional. Its fields do not configure the output device: use
`output:set_volume()` to set the volume, and call `output:info()` to obtain
the PCM format required by `output:write()`.

### `audio.open_input([opts])`

Opens an input device and returns `input`, or `nil, error_message`.

`opts` is optional and may contain:

- `volume`: integer from 0 to 100.
- `sample_rate`: positive integer.
- `channels`: positive integer.
- `bits`: one of 8, 16, 24, or 32.

Omitted format fields use the device defaults. The returned input PCM format
is available through `input:info()`.

## Output object

### `output:info()`

Returns:

```lua
{
  role = "output",
  opened = true,
  sample_rate = 16000,
  channels = 1,
  bits = 16,
  bytes_per_frame = 2,
}
```

`sample_rate`, `channels`, `bits`, and `bytes_per_frame` define the raw PCM
format accepted by `output:write()`.

### `output:set_volume(percent)` / `output:get_volume()`

`set_volume` sets the output volume. `percent` must be an integer from 0 to
100. It returns `true` on success. `get_volume` returns the current integer
volume.

### `output:set_mute(mute)`

Returns `true` without changing the audible output. Use `set_volume(0)` to mute.

### `output:write(pcm)`

Writes a Lua string containing frame-aligned raw PCM in the format returned by
`output:info()`. Returns the number of bytes written, or `nil, error_message`.

### `output:play_tone(freq_hz, duration_ms)`

Plays a sine tone and returns `true`, or `nil, error_message`. Both arguments
must be positive integers. `freq_hz` must be lower than half the output sample
rate. This method supports only 16-bit and 32-bit output PCM formats.

### `output:status()`

Returns output buffering and playback statistics:

| Field | Meaning |
|---|---|
| `queued_bytes` | Audio bytes waiting to play |
| `capacity_bytes` | Maximum buffered audio bytes |
| `queued_ms` | Audio duration waiting to play, in milliseconds |
| `consumed_frames` | Output frames consumed, including silence inserted when audio is unavailable |
| `missing_frames` | Output frames for which audio was unavailable |

Counters begin with the first audio submission and reset when a sample player starts or closes. `stop(voice)` and `clear()` do not reset them. When diagnosing interruptions, compare `missing_frames` before and after active playback; an idle output left open after writing can also accumulate missing frames.

### `output:close()`

Closes the output device.

## Input object

### `input:info()`

Returns the same fields as `output:info()`, with `role = "input"`.

### `input:set_volume(percent)` / `input:get_volume()`

`percent` must be an integer from 0 to 100. `set_volume` records the requested
value and returns `true`; `get_volume` returns that value. It does not change
the recording level.

### `input:read(bytes)`

### `input:read({ bytes = bytes })`

Reads and returns a Lua string containing raw PCM in the format returned by
`input:info()`. The call waits until the requested amount is available or
fails. On failure it returns `nil, error_message`.

### `input:close()`

Closes the input device.

## Player

Create a player from an output device:

```lua
local player = assert(audio.player({ output = output }))
```

### `audio.player({ output = output })`

Returns a player object. `output` must be an open output object.

### `player:play(path_or_uri [, opts])`

Starts playback and returns `true`, or `nil, error_message`. `path_or_uri` may
be a local path or an HTTP/HTTPS URI. Absolute local paths are converted to a
file URI automatically. Set `opts.wait = true` to wait until playback ends.

Starting a new playback stops any current playback on the same player.

### `player:stop()` / `player:pause()` / `player:resume()`

Controls playback and returns `true`, or `nil, error_message`.

### `player:poll()`

Returns:

```lua
{
  state = "idle" | "playing" | "paused" | "stopped" | "finished" | "error",
  running = boolean,
  music_info = { sample_rate = integer, channels = integer, bits = integer, bitrate = integer }, -- optional
}
```

`music_info` is present after media format information becomes available.

### `player:close()`

Stops and closes the player.

## Sample player

`audio.sample_player` plays overlapping game effects and instrument samples. It accepts **signed 16-bit little-endian mono raw PCM**, with no WAV header. Use `audio.player` for encoded files and streams.

### Example

Place a 16 kHz PCM file at `apps/my_game/assets/click.pcm` under the writable storage root:

```lua
local audio, storage, delay = require("audio"), require("storage"), require("delay")
local path = storage.join_path(storage.get_root_dir(), "apps/my_game/assets/click.pcm")
local output = assert(audio.open_output())
local player
local ok, err = pcall(function()
    player = assert(audio.sample_player({ output = output, source_rate = 16000 }))
    assert(player:load(1, storage.read_file(path)))
    assert(player:start())
    local voice = assert(player:trigger(1, 0.5, 0, 300))
    delay.delay_ms(400)
end)
if player then player:close() end
output:close()
assert(ok, err)
```

### Options

| Option | Default | Range |
|---|---|---|
| `output` | Required | Open output object |
| `source_rate` | `16000` | Integer, 8000–48000 Hz; same for all samples |
| `max_voices` | `6` | Integer, 1–16 |
| `sample_budget_bytes` | `2097152` | Integer, 2–4294967295; loaded PCM only |
| `attack_ms`, `release_ms` | `0` | Integer, 0–1000 ms fade-in/fade-out |
| `limit_threshold` | `0` | Finite, 0 ≤ value < 1; 0 disables soft limiting |

### Methods

- `load(id, pcm)`: load before `start()`; up to 64 unique sample IDs. Duplicate IDs, empty/odd-sized PCM and loads exceeding the budget are rejected without changing existing samples.
- `start()`: start once and reserve the output until `close()`. Other players and direct writes cannot use that output meanwhile.
- `trigger(id, gain=1, delay_ms=0, duration_ms=0, rate=1)`: return a handle for one playback. Gain is finite, 0–1; delay is an integer, 0–1000 ms; duration is an integer, 0–60000 ms (0 plays the natural length); rate is finite, 0.25–4. When all voices are occupied, the oldest is replaced.
- `stop(voice)`: cancel one active or delayed playback. Ended or replaced handles are harmless; handles from another player are rejected.
- `clear()`: cancel all playbacks, keeping samples and output ownership.
- `status()`: return `{ active_voices, loaded_samples, sample_bytes, sample_budget_bytes, render_max_us, rendered_blocks, error }`. Voice counts include delayed sounds and may briefly lag requests. Close the player if `error` is not `"ESP_OK"`.
- `close()`: stop, release samples and output ownership; safe to repeat.

Methods return `true` or `nil, error_message`, except `trigger()` and `status()`. Invalid arguments may raise errors. Busy playback can reject trigger/stop requests; check their results. Keep the script running while sounds play, and close the player before its output. Handles keep their player reachable; dropping a handle does not stop its sound.

### Pitch and effects

Rate `2` raises pitch one octave and halves natural duration; `0.5` does the reverse. For a known sample pitch, calculate `rate = 2 ^ ((target_midi - sample_midi) / 12)`. Sample IDs do not themselves represent pitches.

Layer effects by triggering multiple loaded samples with different gains, delays and rates. Keep the returned handles to cancel individual sounds. Output latency also applies to triggers and cancellation.

Samples cannot change after `start()`. Looping, duration-preserving pitch shifts, live gain/pitch adjustment, pan and reverb are not supported. A longer `duration_ms` cannot extend a sample beyond its end. The sample budget excludes Lua strings and other playback memory.

## Recorder

Create a recorder from an input device:

```lua
local recorder = assert(audio.recorder({ input = input }))
```

### `audio.recorder({ input = input })`

Returns a recorder object. `input` must be an open input object.

### `recorder:record(path, opts)`

Records for the requested duration and returns metadata, or `nil, error_message`.
`path` must be a local path ending in `.wav` or `.aac` and must not contain
`..`. The file is replaced if it already exists.

`opts` must contain `duration_ms`, a positive integer. It may also contain:

- `sample_rate`, `channels`, `bits`: output PCM format. Omitted fields use the
  input format.
- `bitrate`: non-negative integer; used only for AAC. Zero or omission selects
  a default bitrate.

For WAV, the result is:

```lua
{ path = string, duration_ms = integer, bytes = integer, format = format }
```

For AAC, the result additionally contains `encoding = "aac"`:

```lua
{ path = string, duration_ms = integer, bytes = integer, encoding = "aac", format = format }
```

`format` is `{ sample_rate, channels, bits, bytes_per_frame }`. AAC requires
16-bit PCM, one or two channels, and a sample rate from 8000 to 96000 Hz.

### `recorder:close()`

Closes the recorder.

## Analyzer

Create an analyzer from an input device:

```lua
local analyzer = assert(audio.analyzer({ input = input }))
```

### `audio.analyzer({ input = input })`

Returns an analyzer object. `input` must be an open input object.

An analyzer and a recorder may be created from the same input, but only one
input-consuming operation can run at a time. A concurrent operation returns
`nil, error_message`.

### `analyzer:read_level([duration_ms])`

### `analyzer:read_level({ duration_ms = duration_ms })`

Captures audio for the requested duration and returns:

```lua
{ rms = integer, peak = integer, duration_ms = integer }
```

The default duration is 100 ms.

### `analyzer:read_spectrum([fft_size [, bands]])`

### `analyzer:read_spectrum({ fft_size = fft_size, bands = bands })`

Returns:

```lua
{
  bands = { integer, ... },
  peak_freq_hz = number,
  peak_db = number,
  rms = integer,
  fft_size = integer,
  band_count = integer,
  sample_rate = integer,
}
```

`bands` contains one level from 0 to 255 for each frequency band. Defaults are
`fft_size = 512` and `bands = 16`. `fft_size` must be a power of two from 64
to 4096; `bands` must be from 1 to `fft_size / 2`.

### `analyzer:close()`

Closes the analyzer.

## Example

```lua
local audio = require("audio")
local storage = require("storage")

local output = assert(audio.open_output())
output:set_volume(80)
local player = assert(audio.player({ output = output }))
local path = storage.join_path(storage.get_root_dir(), "static/test.mp3")

assert(player:play(path, { wait = true }))
assert(player:close())
assert(output:close())
```
