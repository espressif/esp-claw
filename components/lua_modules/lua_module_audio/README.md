# Lua Audio

The `audio` module provides Lua APIs for PCM input and output, media playback,
recording, and basic audio analysis.

```lua
local audio = require("audio")
```

## Resource and error handling

- At most one input device and one output device can be open at a time.
- Close players before their output device, and close recorders and analyzers
  before their input device. Closing a device that is still in use returns
  `nil, "audio device: busy"`.
- Operations that fail at runtime return `nil, error_message`. Invalid Lua
  argument types or values raise a Lua error.
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

Accepted for compatibility. It currently has no audible effect and returns
`true`.

### `output:write(pcm)`

Writes a Lua string containing frame-aligned raw PCM in the format returned by
`output:info()`. Returns the number of bytes written, or `nil, error_message`.

### `output:play_tone(freq_hz, duration_ms)`

Plays a sine tone and returns `true`, or `nil, error_message`. Both arguments
must be positive integers. `freq_hz` must be lower than half the output sample
rate. This method supports only 16-bit and 32-bit output PCM formats.

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
