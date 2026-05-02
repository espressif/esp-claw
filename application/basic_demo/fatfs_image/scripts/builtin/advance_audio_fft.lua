local audio = require("audio")
local bm = require("board_manager")
local delay = require("delay")
local led_strip = require("led_strip")

local LED_GPIO = 14
local LED_COUNT = 16
local BASE_BRIGHTNESS = 0
local SAMPLE_WINDOW_MS = 20
local FRAME_DELAY_MS = 0
local RMS_FLOOR = 120
local RMS_CEIL = 4500
local LOUDNESS_GAMMA = 0.60
local LOUDNESS_BOOST = 1.45
local PEAK_HOLD = 2

local input_codec, input_rate, input_channels, input_bits, input_gain =
    bm.get_audio_codec_input_params("audio_adc")
if not input_codec then
    print("[audio_fft_2] ERROR: get_audio_codec_input_params(audio_adc) failed: " .. tostring(input_rate))
    return
end

local input, in_err = audio.new_input(input_codec, input_rate, input_channels, input_bits, input_gain)
if not input then
    print("[audio_fft_2] ERROR: new_input failed: " .. tostring(in_err))
    return
end

local strip, strip_err = led_strip.new(LED_GPIO, LED_COUNT)
if not strip then
    print("[audio_fft_2] ERROR: led_strip.new failed: " .. tostring(strip_err))
    audio.close(input)
    return
end

local levels = {}
local peak_index = 0
for i = 1, LED_COUNT do
    levels[i] = BASE_BRIGHTNESS
end

local function close_all()
    pcall(function()
        strip:clear()
        strip:refresh()
    end)
    pcall(function()
        strip:close()
    end)
    pcall(audio.close, input)
end

local function clamp(v, lo, hi)
    if v < lo then
        return lo
    end
    if v > hi then
        return hi
    end
    return v
end

local function lerp_int(a, b, t)
    return math.floor(a + (b - a) * t + 0.5)
end

local function loudness_ratio(rms)
    local ratio = (rms - RMS_FLOOR) / (RMS_CEIL - RMS_FLOOR)
    ratio = clamp(ratio, 0, 1)
    ratio = ratio ^ LOUDNESS_GAMMA
    return clamp(ratio * LOUDNESS_BOOST, 0, 1)
end

local function smooth(prev, target)
    if target > prev then
        return math.floor(prev * 0.15 + target * 0.85 + 0.5)
    end
    return math.floor(prev * 0.05 + target * 0.95 + 0.5)
end

print(string.format(
    "[audio_fft_2] mic=%dHz/%dch/%dbit gain=%s led_gpio=%d leds=%d",
    input_rate, input_channels, input_bits, tostring(input_gain), LED_GPIO, LED_COUNT
))

local ok, err = xpcall(function()
    while true do
        local info = audio.mic_read_level(input, SAMPLE_WINDOW_MS)
        local rms = tonumber(info.rms) or 0
        local peak = tonumber(info.peak) or 0
        local ratio = loudness_ratio(rms)
        local active_leds = clamp(math.floor(ratio * LED_COUNT + 0.999), 0, LED_COUNT)
        local global_value = lerp_int(48, 255, ratio)

        if active_leds > peak_index then
            peak_index = active_leds
        elseif peak_index > 0 then
            peak_index = peak_index - PEAK_HOLD
            if peak_index < active_leds then
                peak_index = active_leds
            end
        end

        for i = 1, LED_COUNT do
            local target = BASE_BRIGHTNESS
            if i <= active_leds then
                local pos = (i - 1) / math.max(1, LED_COUNT - 1)
                local shaped = 0.55 + pos * 0.45
                target = clamp(math.floor(global_value * shaped + 0.5), BASE_BRIGHTNESS, 255)
            elseif i == peak_index and peak > 0 then
                target = 180
            end

            levels[i] = clamp(smooth(levels[i], target), BASE_BRIGHTNESS, 255)

            local hue = ((i - 1) * 360) // LED_COUNT
            strip:set_pixel_hsv(i - 1, hue, 255, levels[i])
        end

        strip:refresh()
        delay.delay_ms(FRAME_DELAY_MS)
    end
end, debug.traceback)

close_all()
if not ok then
    error(err)
end
