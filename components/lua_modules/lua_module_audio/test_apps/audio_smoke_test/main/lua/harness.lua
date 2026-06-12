local audio = require("audio")
local audio_test = require("audio_test")

local case = args and args.case or ""
local base = args and args.base or "/fatfs/audio_test"

local function assert_true(value, message)
    if not value then
        error(message or "assert_true failed")
    end
end

local function assert_equal(expected, actual, message)
    if expected ~= actual then
        error(string.format("%s: expected=%s actual=%s", message or "assert_equal failed", tostring(expected), tostring(actual)))
    end
end

local function assert_match(text, needle, message)
    if not string.find(tostring(text), needle, 1, true) then
        error(string.format("%s: text=%s needle=%s", message or "assert_match failed", tostring(text), tostring(needle)))
    end
end

local function expect_error(fn, needle)
    local ok, a, b = pcall(fn)
    if ok and a ~= nil then
        error("expected failure")
    end
    local text = ok and tostring(b) or tostring(a)
    if needle then
        assert_match(text, needle, "unexpected error")
    end
end

local function output_desc(volume)
    return {
        codec = audio_test.output_codec(),
        sample_rate = 48000,
        channels = 2,
        bits = 16,
        volume = volume or 40,
    }
end

local function input_desc(volume)
    return {
        codec = audio_test.input_codec(),
        sample_rate = 48000,
        channels = 2,
        bits = 16,
        volume = volume or 60,
    }
end

local cases = {}

function cases.api()
    assert_equal("function", type(audio.new_output), "new_output")
    assert_equal("function", type(audio.new_input), "new_input")
    assert_equal("function", type(audio.player), "player")
    assert_equal("function", type(audio.recorder), "recorder")
    assert_equal("function", type(audio.analyzer), "analyzer")
    print("api ok")
end

function cases.invalid_args()
    expect_error(function()
        return audio.new_output({ sample_rate = 16000, channels = 1, bits = 16 })
    end, "codec is required")
    expect_error(function()
        return audio.new_output({ codec = audio_test.output_codec(), sample_rate = 16000, channels = 1, bits = 16, volume = 101 })
    end, "volume must be 0..100")
    expect_error(function()
        return audio.new_output({ codec = audio_test.output_codec(), sample_rate = 16000, channels = 1, bits = 12 })
    end, "invalid format")
    print("invalid_args ok")
end

function cases.output_device()
    audio_test.reset_stats()
    local output = assert(audio.new_output(output_desc(35)))
    local info = output:info()
    assert_equal("output", info.role, "output role")
    assert_equal(true, info.opened, "output opened")
    assert_equal(48000, info.sample_rate, "output sample rate")
    assert_equal(2, info.channels, "output channels")
    assert_equal(16, info.bits, "output bits")
    assert_equal(4, info.bytes_per_frame, "output frame bytes")
    assert_equal(35, output:get_volume(), "initial volume")
    assert(output:set_volume(72))
    assert_equal(72, output:get_volume(), "updated volume")
    assert(output:set_mute(true))
    assert(output:set_mute(false))
    assert(output:write(string.rep("\0", 128)))
    assert(output:play_tone(440, 20))
    local stats = audio_test.stats()
    assert_true(stats.output_write_bytes >= 128, "write bytes")
    assert(output:close())
    print("output_device ok")
end

function cases.input_device()
    audio_test.reset_stats()
    local input = assert(audio.new_input(input_desc(55)))
    local info = input:info()
    assert_equal("input", info.role, "input role")
    assert_equal(true, info.opened, "input opened")
    assert_equal(48000, info.sample_rate, "input sample rate")
    assert_equal(2, info.channels, "input channels")
    assert_equal(16, info.bits, "input bits")
    assert_equal(4, info.bytes_per_frame, "input frame bytes")
    assert_equal(55, input:get_volume(), "input volume")
    assert(input:set_volume(25))
    assert_equal(25, input:get_volume(), "updated input volume")
    local data = assert(input:read(64))
    assert_equal(64, #data, "input read size")
    local stats = audio_test.stats()
    assert_true(stats.input_read_bytes >= 64, "read bytes")
    assert(input:close())
    print("input_device ok")
end

function cases.player_lifecycle()
    local output = assert(audio.new_output(output_desc(45)))
    local player = assert(audio.player({ output = output }))
    local info = output:info()
    assert_equal(true, info.opened, "output remains open")
    expect_error(function()
        return output:close()
    end, "busy")
    assert(player:close())
    assert(output:close())
    print("player_lifecycle ok")
end

function cases.analyzer()
    audio_test.reset_stats()
    local input = assert(audio.new_input(input_desc(50)))
    local analyzer = assert(audio.analyzer({ input = input }))
    local level = analyzer:read_level({ duration_ms = 20 })
    assert_true(level.rms >= 0, "level rms")
    assert_true(level.peak >= 0, "level peak")
    local spectrum = analyzer:read_spectrum({ fft_size = 64, bands = 8 })
    assert_equal(64, spectrum.fft_size, "fft size")
    assert_equal(8, spectrum.band_count, "band count")
    assert_equal(8, #spectrum.bands, "bands length")
    assert_true(spectrum.sample_rate > 0, "spectrum sample rate")
    assert(analyzer:close())
    assert(input:close())
    print("analyzer ok")
end

function cases.recorder_wav()
    audio_test.reset_stats()
    local input = assert(audio.new_input(input_desc(50)))
    local recorder = assert(audio.recorder({ input = input }))
    local path = base .. "/rec.wav"
    local info = assert(recorder:record(path, { duration_ms = 20, sample_rate = 16000, channels = 1, bits = 16 }))
    assert_equal(path, info.path, "record path")
    assert_equal(20, info.duration_ms, "record duration")
    assert_true(info.bytes > 0, "record bytes")
    assert_equal(16000, info.format.sample_rate, "record sample rate")
    assert_equal(1, info.format.channels, "record channels")
    assert_equal(16, info.format.bits, "record bits")
    local file = assert(io.open(path, "rb"))
    local header = file:read(4)
    file:close()
    assert_equal("RIFF", header, "wav header")
    assert(recorder:close())
    assert(input:close())
    print("recorder_wav ok")
end

local fn = cases[case]
if not fn then
    error("unknown case: " .. tostring(case))
end

fn()
