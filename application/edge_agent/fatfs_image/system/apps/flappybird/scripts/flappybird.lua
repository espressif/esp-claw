local display = require("display")
local screen, screen_info

local function center_text(x, y, w, h, text, options)
    local tw, th = screen:measure_text(text, options)
    screen:text(x + math.max(0, (w - tw) // 2), y + math.max(0, (h - th) // 2), text, options)
end
local delay = require("delay")
local audio_ok, audio = pcall(require, "audio")

local button_ok, button = pcall(require, "button")

local a = type(args) == "table" and args or {}
local function int_arg(key, default)
    local value = a[key]
    if type(value) == "number" then
        return math.floor(value)
    end
    return default
end

local function str_arg(key, default)
    local value = a[key]
    if type(value) == "string" and value ~= "" then
        return value
    end
    return default
end

local BUTTON_PIN = int_arg("pin", int_arg("button_gpio", 0))
local BUTTON_ACTIVE_LEVEL = int_arg("active_level", 0)
local HAS_BUTTON_ARG = type(a.pin) == "number" or type(a.button_gpio) == "number"
local INPUT_MODE = str_arg("input_mode", HAS_BUTTON_ARG and "button" or "touch")
local FRAME_MS = 33
local RUN_TIME_MS = int_arg("run_time_ms", 180000)
local OUTPUT_SAMPLE_RATE = int_arg("sample_rate_hz", 16000)

local GRAVITY = 0.82
local FLAP_VELOCITY = -7.4
local PIPE_SPEED = 3.15
local PIPE_WIDTH = 46
local PIPE_CAP_OVERHANG = 5
local PIPE_GAP = 126
local PIPE_GAP_MIN = 112
local PIPE_SPAWN_MS = 1750
local GROUND_HEIGHT = 42
local BIRD_RADIUS = 17
local BIRD_DRAW_SCALE = 0.82
local BIRD_X_RATIO = 0.26
local CLOUD_COUNT = 3
local PIPE_MARGIN = 58
local PIPE_STEP = 8
local MAX_PIPE_SHIFT = 32
local SOUND_VOLUME = 90
local UAC_FLUSH_PCM_BYTES = 4000 -- min PCM bytes for UAC host write
local SFX_FLAP_HZ, SFX_FLAP_MS = 920, 90
local SFX_SCORE_HZ, SFX_SCORE_MS = 1320, 100
local SFX_CRASH_HZ, SFX_CRASH_MS = 180, 350

if OUTPUT_SAMPLE_RATE <= 8000 then
    SFX_FLAP_HZ = 660
    SFX_SCORE_HZ = 880
end

local SKY_R, SKY_G, SKY_B = 102, 205, 237
local SKY_LOW_R, SKY_LOW_G, SKY_LOW_B = 147, 224, 223
local SUN_R, SUN_G, SUN_B = 255, 215, 115
local CLOUD_R, CLOUD_G, CLOUD_B = 247, 252, 249
local PIPE_R, PIPE_G, PIPE_B = 42, 181, 104
local PIPE_SHADE_R, PIPE_SHADE_G, PIPE_SHADE_B = 23, 123, 78
local PIPE_CAP_R, PIPE_CAP_G, PIPE_CAP_B = 65, 211, 126
local PIPE_OUTLINE_R, PIPE_OUTLINE_G, PIPE_OUTLINE_B = 14, 91, 68
local GROUND_R, GROUND_G, GROUND_B = 95, 188, 103
local DIRT_R, DIRT_G, DIRT_B = 221, 170, 83
local ACCENT_R, ACCENT_G, ACCENT_B = 255, 111, 66
local TEXT_R, TEXT_G, TEXT_B = 17, 45, 67
local PANEL_R, PANEL_G, PANEL_B = 251, 252, 244
local PANEL_BORDER_R, PANEL_BORDER_G, PANEL_BORDER_B = 50, 119, 139
local DANGER_R, DANGER_G, DANGER_B = 221, 70, 69
local input_mode = "none"
local button_handle = nil
local button_active_level = BUTTON_ACTIVE_LEVEL
local button_last_level = 1
local audio_output = nil
local sfx_cache = {}
local sfx_pending = nil

local function build_tone_pcm(freq_hz, duration_ms)
    local rate = OUTPUT_SAMPLE_RATE
    local frames = math.floor(rate * duration_ms / 1000)
    if frames <= 0 then
        return string.rep("\0", UAC_FLUSH_PCM_BYTES)
    end

    local amp = 18000
    local phase = 0
    local step = 2 * math.pi * freq_hz / rate
    local chunks = {}
    for i = 1, frames do
        local s = math.floor(math.sin(phase) * amp + 0.5)
        phase = phase + step
        if s > 32767 then
            s = 32767
        elseif s < -32768 then
            s = -32768
        end
        local u = s < 0 and (s + 65536) or s
        chunks[i] = string.char(u % 256, math.floor(u / 256) % 256)
    end

    local pcm = table.concat(chunks)
    if #pcm < UAC_FLUSH_PCM_BYTES then
        pcm = pcm .. string.rep("\0", UAC_FLUSH_PCM_BYTES - #pcm)
    end
    return pcm
end

local function init_sfx_cache()
    sfx_cache.flap = build_tone_pcm(SFX_FLAP_HZ, SFX_FLAP_MS)
    sfx_cache.score = build_tone_pcm(SFX_SCORE_HZ, SFX_SCORE_MS)
    sfx_cache.crash = build_tone_pcm(SFX_CRASH_HZ, SFX_CRASH_MS)
    sfx_cache.ready = build_tone_pcm(660, 120)
end

local function request_sfx(kind)
    local pcm = sfx_cache[kind]
    if pcm then
        sfx_pending = { pcm = pcm, tag = kind }
    end
end

local function drain_sfx()
    if not sfx_pending or not audio_output then
        return
    end

    local pending = sfx_pending
    local ok, err = audio_output:write(pending.pcm)
    if ok then
        sfx_pending = nil
        return
    end

    if err and not tostring(err):find("busy", 1, true) then
        print(string.format("[flappybird] WARN: %s write failed: %s",
            pending.tag or "sfx", tostring(err)))
        sfx_pending = nil
    end
end

local color_cache = {}

local function rgb(r, g, b)
    local key = (r << 16) | (g << 8) | b
    local color = color_cache[key]
    if color == nil then
        color = display.color(r, g, b)
        color_cache[key] = color
    end
    return color
end

local BIRD_COLORS = {
    beak = rgb(244, 82, 45),
    beak_light = rgb(255, 119, 65),
    mouth = rgb(117, 27, 39),
    gold = rgb(241, 166, 4),
    yellow = rgb(255, 215, 8),
    light = rgb(255, 233, 72),
    belly = rgb(255, 241, 121),
    iris = rgb(76, 190, 218),
    pupil = rgb(28, 23, 24),
    white = rgb(250, 250, 239),
}

local BIRD_SHAPES = {
    tail_top_outer = { -26, -10, -36, -22, -31, -24, -21, -18, -14, -8, -20, -2, -34, -4 },
    tail_top_inner = { -26, -11, -34, -21, -30, -22, -21, -17, -16, -8, -21, -4, -32, -6 },
    tail_bottom_outer = { -26, 14, -36, 8, -30, 6, -16, 8, -14, 16, -22, 23, -33, 20 },
    tail_bottom_inner = { -26, 14, -34, 10, -29, 8, -18, 10, -16, 16, -23, 20, -32, 18 },
    crest = { 2, -29, -5, -30, 0, -34, 7, -32, 13, -25, 9, -21, -1, -22 },
    body_outer = { 0, 0, -24, -10, -12, -22, 8, -24, 22, -18, 28, -8, 28, 8, 18, 20, 2, 24, -16, 20, -25, 8 },
    body_inner = { 0, -2, -22, -10, -10, -20, 8, -22, 20, -16, 26, -7, 26, 6, 16, 17, 2, 21, -14, 17, -23, 6 },
    breast = { 11, 14, -5, 10, 12, 5, 26, 4, 18, 17, 3, 21, -7, 17 },
    wing_outer = { -18, 0, -25, -10, -14, -8, -6, 3, -14, 13, -25, 8 },
    wing_inner = { -19, -1, -23, -8, -15, -6, -9, 2, -15, 9, -23, 6 },
    beak_top = { 32, -4, 22, -7, 34, -8, 40, -4, 34, 1, 22, 2 },
    beak_bottom = { 31, 7, 22, 2, 34, 2, 38, 7, 32, 11, 22, 9 },
}

local display_ok, display_info = pcall(display.open)
if not display_ok then
    print("[flappybird] ERROR: display init failed: " .. tostring(display_info))
    return
end
screen = display_info
screen_info = screen:info()

local screen_created = true

local function cleanup()
    if button_handle then
        pcall(button.off, button_handle)
        pcall(button.close, button_handle)
        button_handle = nil
    end

    if audio_output then
        pcall(audio_output.close, audio_output)
        audio_output = nil
    end

    if screen_created then
        pcall(screen.close, screen)
        screen_created = false
    end
end

local width = screen_info.width
local height = screen_info.height

if width <= 0 or height <= 0 then
    print("[flappybird] ERROR: invalid display size after init")
    cleanup()
    return
end

local play_top = 0
local play_bottom = height - GROUND_HEIGHT
local play_height = play_bottom - play_top
local bird_x = math.floor(width * BIRD_X_RATIO)
local score = 0
local best_score = 0
local frame_count = 0
local state = "title"
local bird_y = math.floor(play_height * 0.45)
local bird_vy = 0
local spawn_timer_ms = 0
local pipes = {}
local cloud_offsets = {}
local particles = {}
local touch_down = false
local last_gap_top = nil

math.randomseed(os.time() + width * 13 + height * 17)

for i = 1, CLOUD_COUNT do
    cloud_offsets[i] = {
        x = ((i - 1) * width) // CLOUD_COUNT + math.random(0, 24),
        y = 42 + math.random(0, math.max(10, math.floor(play_height * 0.20))),
        size = 9 + math.random(0, 7),
        speed = 0.18 + math.random() * 0.24,
    }
end

local function clamp(v, min_v, max_v)
    if v < min_v then
        return min_v
    end
    if v > max_v then
        return max_v
    end
    return v
end

local function snap_step(v, step)
    return (v // step) * step
end

local function new_pipe(x)
    local gap = math.max(PIPE_GAP_MIN, PIPE_GAP - math.min(score, PIPE_GAP - PIPE_GAP_MIN))
    local min_gap_top = PIPE_MARGIN
    local max_gap_top = play_bottom - gap - PIPE_MARGIN
    local base_gap_top

    if last_gap_top == nil then
        local range = math.max(0, max_gap_top - min_gap_top)
        base_gap_top = min_gap_top + math.random(0, range)
    else
        local shift = math.random(-MAX_PIPE_SHIFT, MAX_PIPE_SHIFT)
        base_gap_top = last_gap_top + shift
    end

    local gap_top = clamp(base_gap_top, min_gap_top, max_gap_top)
    gap_top = snap_step(gap_top, PIPE_STEP)
    gap_top = clamp(gap_top, min_gap_top, max_gap_top)
    last_gap_top = gap_top

    return {
        x = x,
        gap_top = gap_top,
        gap_bottom = gap_top + gap,
        scored = false,
    }
end

local function reset_round(next_state)
    score = 0
    state = next_state or "title"
    bird_y = math.floor(play_height * (state == "title" and 0.68 or 0.56))
    bird_vy = 0
    spawn_timer_ms = 0
    last_gap_top = nil
    particles = {}
    pipes = { new_pipe(state == "title" and width - PIPE_WIDTH - 18 or width + 84) }
end

local function add_particles(count, burst)
    for _ = 1, count do
        particles[#particles + 1] = {
            x = bird_x - 24 + math.random(-3, 4),
            y = bird_y + math.random(-7, 7),
            vx = burst and math.random(-18, 14) / 10 or math.random(-18, -8) / 10,
            vy = burst and math.random(-18, 18) / 10 or math.random(-8, 8) / 10,
            radius = math.random(2, 4),
            ttl = burst and 16 or 10,
            color = math.random(0, 1) == 0 and BIRD_COLORS.gold or BIRD_COLORS.light,
        }
    end
end

local function flap()
    bird_vy = FLAP_VELOCITY
    add_particles(4, false)
    request_sfx("flap")
end

local function update_particles()
    for i = #particles, 1, -1 do
        local particle = particles[i]
        particle.x = particle.x + particle.vx
        particle.y = particle.y + particle.vy
        particle.vy = particle.vy + 0.08
        particle.ttl = particle.ttl - 1
        if particle.ttl <= 0 then
            table.remove(particles, i)
        end
    end
end

local function draw_cloud(x, y, size)
    local cloud_color = rgb(CLOUD_R, CLOUD_G, CLOUD_B)
    screen:fill_circle(x, y, size, cloud_color)
    screen:fill_circle(x + size, y - 3, math.floor(size * 0.82), cloud_color)
    screen:fill_circle(x + size * 2 - 3, y + 1, math.floor(size * 0.66), cloud_color)
    screen:fill_round_rect(x, y - math.floor(size * 0.45), size * 2, size, math.floor(size * 0.45), cloud_color)
end

local function draw_background()
    local low_sky_y = math.floor(play_bottom * 0.56)
    screen:fill_rect(0, low_sky_y, width, play_bottom - low_sky_y, rgb(SKY_LOW_R, SKY_LOW_G, SKY_LOW_B))
    screen:fill_circle(width - 46, 48, 25, rgb(255, 231, 155))
    screen:fill_circle(width - 46, 48, 17, rgb(SUN_R, SUN_G, SUN_B))

    for i = 1, CLOUD_COUNT do
        local cloud = cloud_offsets[i]
        local drift = math.floor((frame_count * cloud.speed + cloud.x) % (width + 64)) - 32
        draw_cloud(drift, cloud.y, cloud.size)
    end

    local horizon = play_bottom - 58
    screen:fill_triangle(0, play_bottom, math.floor(width * 0.18), horizon + 8, math.floor(width * 0.42), play_bottom, rgb(100, 191, 170))
    screen:fill_triangle(math.floor(width * 0.24), play_bottom, math.floor(width * 0.52), horizon - 10, math.floor(width * 0.82), play_bottom, rgb(82, 178, 160))
    screen:fill_triangle(math.floor(width * 0.64), play_bottom, math.floor(width * 0.86), horizon + 12, width, play_bottom, rgb(100, 191, 170))
    screen:fill_circle(32, play_bottom - 17, 25, rgb(79, 163, 121))
    screen:fill_circle(78, play_bottom - 12, 22, rgb(79, 163, 121))
    screen:fill_circle(width - 54, play_bottom - 15, 28, rgb(79, 163, 121))

    screen:fill_rect(0, play_bottom, width, GROUND_HEIGHT, rgb(GROUND_R, GROUND_G, GROUND_B))
    screen:fill_rect(0, play_bottom + 10, width, GROUND_HEIGHT - 10, rgb(DIRT_R, DIRT_G, DIRT_B))
    screen:fill_rect(0, play_bottom + 10, width, 4, rgb(238, 195, 108))

    local stripe_w = 18
    for x = 0, width + stripe_w, stripe_w * 2 do
        local offset = (frame_count * 2) % (stripe_w * 2)
        screen:fill_round_rect(x - offset, play_bottom + 21, stripe_w, 5, 2, rgb(239, 193, 99))
    end
end

local function draw_pipe(pipe)
    local x = math.floor(pipe.x)
    local top_h = pipe.gap_top
    local bottom_y = pipe.gap_bottom
    local bottom_h = play_bottom - bottom_y
    local outline = rgb(PIPE_OUTLINE_R, PIPE_OUTLINE_G, PIPE_OUTLINE_B)
    local pipe_color = rgb(PIPE_R, PIPE_G, PIPE_B)
    local cap_color = rgb(PIPE_CAP_R, PIPE_CAP_G, PIPE_CAP_B)
    local shade = rgb(PIPE_SHADE_R, PIPE_SHADE_G, PIPE_SHADE_B)
    local highlight = rgb(122, 231, 158)
    local cap_x = x - PIPE_CAP_OVERHANG
    local cap_w = PIPE_WIDTH + PIPE_CAP_OVERHANG * 2

    screen:fill_round_rect(x, -8, PIPE_WIDTH, top_h + 8, 8, outline)
    screen:fill_round_rect(x + 2, -8, PIPE_WIDTH - 4, top_h + 6, 6, pipe_color)
    screen:fill_rect(x + PIPE_WIDTH - 9, 0, 7, math.max(0, top_h - 10), shade)
    screen:fill_round_rect(x + 6, 0, 5, math.max(0, top_h - 18), 2, highlight)
    screen:fill_round_rect(cap_x, top_h - 16, cap_w, 16, 6, outline)
    screen:fill_round_rect(cap_x + 2, top_h - 14, cap_w - 4, 12, 4, cap_color)

    screen:fill_round_rect(x, bottom_y, PIPE_WIDTH, bottom_h + 8, 8, outline)
    screen:fill_round_rect(x + 2, bottom_y + 2, PIPE_WIDTH - 4, bottom_h + 6, 6, pipe_color)
    screen:fill_rect(x + PIPE_WIDTH - 9, bottom_y + 10, 7, math.max(0, bottom_h - 10), shade)
    screen:fill_round_rect(x + 6, bottom_y + 18, 5, math.max(0, bottom_h - 18), 2, highlight)
    screen:fill_round_rect(cap_x, bottom_y, cap_w, 16, 6, outline)
    screen:fill_round_rect(cap_x + 2, bottom_y + 2, cap_w - 4, 12, 4, cap_color)
end

local function bird_scale(value)
    if value >= 0 then
        return math.floor(value * BIRD_DRAW_SCALE + 0.5)
    end
    return math.ceil(value * BIRD_DRAW_SCALE - 0.5)
end

local function draw_bird_fan(shape, color, y_shift)
    local shift = y_shift or 0
    local cx = bird_scale(shape[1])
    local cy = bird_scale(shape[2] + shift)
    for i = 3, #shape, 2 do
        local next_i = i + 2
        if next_i > #shape then
            next_i = 3
        end
        screen:fill_triangle(cx, cy, bird_scale(shape[i]), bird_scale(shape[i + 1] + shift), bird_scale(shape[next_i]), bird_scale(shape[next_i + 1] + shift), color)
    end
end

local function draw_bird()
    local c = BIRD_COLORS
    local wing_up = ((frame_count // 3) % 4) < 2
    local wing_shift = wing_up and -3 or 3
    local bob = state == "title" and math.floor(math.sin(frame_count * 0.14) * 4) or 0

    screen:save()
    screen:translate(bird_x, math.floor(bird_y) + bob)

    draw_bird_fan(BIRD_SHAPES.tail_top_outer, c.gold)
    screen:fill_circle(bird_scale(-31), bird_scale(-15), bird_scale(8), c.gold)
    draw_bird_fan(BIRD_SHAPES.tail_top_inner, c.light)
    screen:fill_circle(bird_scale(-30), bird_scale(-15), bird_scale(6), c.light)
    draw_bird_fan(BIRD_SHAPES.tail_bottom_outer, c.gold)
    screen:fill_circle(bird_scale(-31), bird_scale(14), bird_scale(6), c.gold)
    draw_bird_fan(BIRD_SHAPES.tail_bottom_inner, c.yellow)
    screen:fill_circle(bird_scale(-30), bird_scale(14), bird_scale(4), c.yellow)

    draw_bird_fan(BIRD_SHAPES.crest, c.gold)
    screen:fill_circle(bird_scale(0), bird_scale(-32), bird_scale(3), c.gold)
    screen:fill_triangle(bird_scale(-2), bird_scale(-29), bird_scale(1), bird_scale(-33), bird_scale(11), bird_scale(-23), c.light)

    draw_bird_fan(BIRD_SHAPES.body_outer, c.gold)
    draw_bird_fan(BIRD_SHAPES.body_inner, c.yellow)
    screen:fill_circle(bird_scale(12), bird_scale(-8), bird_scale(16), c.gold)
    screen:fill_circle(bird_scale(12), bird_scale(-9), bird_scale(14), c.yellow)
    draw_bird_fan(BIRD_SHAPES.breast, c.belly)

    draw_bird_fan(BIRD_SHAPES.wing_outer, c.gold, wing_shift)
    screen:fill_circle(bird_scale(-20), bird_scale(-1 + wing_shift), bird_scale(8), c.gold)
    draw_bird_fan(BIRD_SHAPES.wing_inner, c.light, wing_shift)
    screen:fill_circle(bird_scale(-19), bird_scale(-2 + wing_shift), bird_scale(5), c.light)
    screen:line(bird_scale(-22), bird_scale(4 + wing_shift), bird_scale(-12), bird_scale(8 + wing_shift), c.gold)
    screen:line(bird_scale(-20), bird_scale(8 + wing_shift), bird_scale(-13), bird_scale(10 + wing_shift), c.gold)

    draw_bird_fan(BIRD_SHAPES.beak_top, c.beak_light)
    draw_bird_fan(BIRD_SHAPES.beak_bottom, c.beak)
    screen:fill_triangle(bird_scale(22), bird_scale(1), bird_scale(35), bird_scale(2), bird_scale(22), bird_scale(5), c.mouth)

    screen:fill_round_rect(bird_scale(10), bird_scale(-18), bird_scale(14), bird_scale(20), bird_scale(7), c.white)
    screen:fill_round_rect(bird_scale(18), bird_scale(-14), bird_scale(6), bird_scale(14), bird_scale(3), c.iris)
    screen:fill_round_rect(bird_scale(21), bird_scale(-10), bird_scale(5), bird_scale(9), bird_scale(2), c.pupil)
    screen:fill_rect(bird_scale(21), bird_scale(-11), bird_scale(3), bird_scale(3), c.white)

    screen:restore()
end

local function draw_particles()
    for i = 1, #particles do
        local particle = particles[i]
        screen:fill_circle(math.floor(particle.x), math.floor(particle.y), particle.radius, particle.color)
    end
end

local function draw_scoreboard()
    local pill_w = 70
    local pill_x = width - pill_w - 10
    screen:fill_round_rect(pill_x + 2, 12, pill_w, 30, 10, rgb(38, 116, 138))
    screen:fill_round_rect(pill_x, 10, pill_w, 30, 10, rgb(PANEL_R, PANEL_G, PANEL_B))
    screen:text(pill_x + 8, 14, "BEST", { color = rgb(74, 116, 130), font_size = 9 })
    screen:text(pill_x + 43, 13, tostring(best_score), { color = rgb(TEXT_R, TEXT_G, TEXT_B), font_size = 15 })

    if state == "playing" then
        local score_text = tostring(score)
        local options = { color = rgb(255, 255, 255), font_size = 34 }
        local score_w = screen:measure_text(score_text, options)
        local score_x = (width - score_w) // 2
        screen:text(score_x + 2, 10, score_text, { color = rgb(35, 110, 132), font_size = 34 })
        screen:text(score_x, 8, score_text, options)
    end
end

local function draw_center_panel(title, detail, action, accent_color)
    local panel_w = math.min(width - 32, 276)
    local panel_h = 116
    local panel_x = (width - panel_w) // 2
    local panel_y = state == "title" and 52 or 70

    screen:fill_round_rect(panel_x + 5, panel_y + 6, panel_w, panel_h, 16, rgb(40, 116, 137))
    screen:fill_round_rect(panel_x, panel_y, panel_w, panel_h, 16, rgb(PANEL_R, PANEL_G, PANEL_B))
    screen:stroke_round_rect(panel_x, panel_y, panel_w, panel_h, 16, rgb(PANEL_BORDER_R, PANEL_BORDER_G, PANEL_BORDER_B))
    screen:fill_round_rect(panel_x + 16, panel_y + 13, 46, 6, 3, accent_color)
    center_text(panel_x + 14, panel_y + 22, panel_w - 28, 30, title, { color = rgb(TEXT_R, TEXT_G, TEXT_B), font_size = 24 })
    center_text(panel_x + 16, panel_y + 52, panel_w - 32, 18, detail, { color = rgb(76, 111, 124), font_size = 12 })

    local button_w = math.min(170, panel_w - 48)
    local button_x = panel_x + (panel_w - button_w) // 2
    screen:fill_round_rect(button_x, panel_y + 79, button_w, 27, 10, accent_color)
    center_text(button_x, panel_y + 79, button_w, 27, action, { color = rgb(255, 255, 255), font_size = 13 })
end

local function action_label(verb)
    if input_mode == "display_touch" then
        return "TAP TO " .. string.upper(verb)
    end
    if input_mode == "button" then
        return "PRESS TO " .. string.upper(verb)
    end
    return string.upper(verb)
end

local function render()
    screen:begin({ clear = rgb(SKY_R, SKY_G, SKY_B) })
    draw_background()

    for i = 1, #pipes do
        draw_pipe(pipes[i])
    end

    draw_particles()
    draw_bird()
    draw_scoreboard()

    if state == "title" then
        draw_center_panel("FLAPPY FLIGHT", "THREAD THE GATES", action_label("fly"), rgb(ACCENT_R, ACCENT_G, ACCENT_B))
    elseif state == "crashed" then
        draw_center_panel("ROUND OVER", "SCORE " .. tostring(score) .. "  /  BEST " .. tostring(best_score), action_label("retry"), rgb(DANGER_R, DANGER_G, DANGER_B))
    end

    screen:present()
end

local function circle_rect_hit(cx, cy, radius, rx, ry, rw, rh)
    local nearest_x = math.max(rx, math.min(cx, rx + rw))
    local nearest_y = math.max(ry, math.min(cy, ry + rh))
    local dx = cx - nearest_x
    local dy = cy - nearest_y
    return dx * dx + dy * dy <= radius * radius
end

local function set_crashed()
    if state == "crashed" then
        return
    end
    if score > best_score then
        best_score = score
    end
    add_particles(10, true)
    request_sfx("crash")
    state = "crashed"
end

local function update_playing()
    bird_vy = bird_vy + GRAVITY
    bird_y = bird_y + bird_vy
    spawn_timer_ms = spawn_timer_ms + FRAME_MS

    if spawn_timer_ms >= PIPE_SPAWN_MS then
        spawn_timer_ms = spawn_timer_ms - PIPE_SPAWN_MS
        pipes[#pipes + 1] = new_pipe(width + PIPE_WIDTH + 8)
    end

    for i = #pipes, 1, -1 do
        local pipe = pipes[i]
        pipe.x = pipe.x - PIPE_SPEED - math.min(score * 0.04, 1.0)

        if not pipe.scored and pipe.x + PIPE_WIDTH < bird_x then
            pipe.scored = true
            score = score + 1
            if score > best_score then
                best_score = score
            end
            request_sfx("score")
        end

        if pipe.x + PIPE_WIDTH < -4 then
            table.remove(pipes, i)
        elseif circle_rect_hit(bird_x + 3, bird_y, BIRD_RADIUS, pipe.x - PIPE_CAP_OVERHANG, 0, PIPE_WIDTH + PIPE_CAP_OVERHANG * 2, pipe.gap_top)
            or circle_rect_hit(bird_x + 3, bird_y, BIRD_RADIUS, pipe.x - PIPE_CAP_OVERHANG, pipe.gap_bottom, PIPE_WIDTH + PIPE_CAP_OVERHANG * 2, play_bottom - pipe.gap_bottom) then
            set_crashed()
        end
    end

    if bird_y - BIRD_RADIUS <= play_top or bird_y + BIRD_RADIUS >= play_bottom then
        set_crashed()
    end
end

local function init_input()
    if INPUT_MODE ~= "button" and screen_info.touch_available then
        input_mode = "display_touch"
        return true
    end

    if not button_ok then
        print("[flappybird] ERROR: require(button) failed")
        return false
    end

    local button_err
    button_handle, button_err = button.new(BUTTON_PIN, button_active_level)
    if not button_handle then
        print("[flappybird] ERROR: button.new failed on gpio " .. tostring(BUTTON_PIN) .. ": " .. tostring(button_err))
        return false
    end

    local level, level_err = button.get_key_level(button_handle)
    if level == nil then
        print("[flappybird] ERROR: button.get_key_level failed: " .. tostring(level_err))
        cleanup()
        return false
    end
    button_last_level = level

    input_mode = "button"
    return true
end

local function consume_input_tap()
    if input_mode == "display_touch" then
        local down = #screen:touch().points > 0
        local tapped = down and not touch_down
        touch_down = down
        return tapped
    end

    if input_mode == "button" then
        local level, level_err = button.get_key_level(button_handle)
        if level == nil then
            print("[flappybird] ERROR: button.get_key_level failed: " .. tostring(level_err))
            return nil
        end

        local tapped = level == button_active_level and button_last_level ~= button_active_level
        button_last_level = level
        return tapped
    end

    return false
end

local function init_audio()
    if not audio_ok then
        print("[flappybird] WARN: require(audio) failed")
        return
    end

    local ok_out, output, output_err = pcall(function()
        return audio.open_output({
            sample_rate = OUTPUT_SAMPLE_RATE,
            volume = SOUND_VOLUME,
        })
    end)
    if not ok_out then
        print("[flappybird] WARN: audio init failed: " .. tostring(output))
        return
    end
    if not output then
        print("[flappybird] WARN: audio init failed: " .. tostring(output_err))
        return
    end

    local info = output:info()
    print(string.format("[flappybird] audio %dHz/%dch/%dbit (requested %dHz)",
        info.sample_rate, info.channels, info.bits, OUTPUT_SAMPLE_RATE))

    audio_output = output
    init_sfx_cache()
    request_sfx("ready")
end

local run_ok, run_err = xpcall(function()
    if not init_input() then
        return
    end

    init_audio()
    reset_round("title")

    print(string.format("[flappybird] ready screen=%dx%d run_ms=%d", width, height, RUN_TIME_MS))
    if input_mode == "display_touch" then
        print("[flappybird] display touch ready, tap anywhere to flap, tap after crashing to restart")
    else
        print(string.format("[flappybird] button gpio=%d active_level=%d", BUTTON_PIN, button_active_level))
    end

    for _ = 1, RUN_TIME_MS // FRAME_MS do
        drain_sfx()

        local tapped = consume_input_tap()
        if tapped == nil then
            break
        end

        if tapped then
            if state == "title" then
                reset_round("playing")
                flap()
            elseif state == "playing" then
                flap()
            elseif state == "crashed" then
                reset_round("playing")
                flap()
            end
        end

        if state == "playing" then
            update_playing()
        end

        update_particles()
        render()
        frame_count = frame_count + 1
        delay.delay_ms(FRAME_MS)
    end
end, debug.traceback)

cleanup()
if not run_ok then
    print("[flappybird] ERROR: " .. tostring(run_err))
end
print("[flappybird] done")
