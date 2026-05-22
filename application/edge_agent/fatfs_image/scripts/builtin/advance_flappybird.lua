local bm = require("board_manager")
local display = require("display")
local delay = require("delay")
local audio_ok, audio = pcall(require, "audio")

local lcd_touch_ok, lcd_touch = pcall(require, "lcd_touch")
local button_ok, button = pcall(require, "button")

local FRAME_MS = 33
local RUN_TIME_MS = 180000
local GRAVITY = 1.22
local FLAP_VELOCITY = -7.2
local PIPE_SPEED = 3.8
local PIPE_WIDTH = 34
local PIPE_GAP = 74
local PIPE_SPAWN_MS = 1500
local GROUND_HEIGHT = 28
local BIRD_RADIUS = 10
local BIRD_X_RATIO = 0.28
local CLOUD_COUNT = 4
local PIPE_MARGIN = 42
local PIPE_STEP = 10
local MAX_PIPE_SHIFT = 26
local SOUND_VOLUME = 90

local SKY_R, SKY_G, SKY_B = 138, 213, 255
local SUN_R, SUN_G, SUN_B = 255, 223, 120
local CLOUD_R, CLOUD_G, CLOUD_B = 248, 252, 255
local PIPE_R, PIPE_G, PIPE_B = 67, 166, 74
local PIPE_SHADE_R, PIPE_SHADE_G, PIPE_SHADE_B = 44, 118, 55
local PIPE_CAP_R, PIPE_CAP_G, PIPE_CAP_B = 102, 204, 96
local GROUND_R, GROUND_G, GROUND_B = 217, 182, 92
local DIRT_R, DIRT_G, DIRT_B = 158, 112, 56
local BIRD_R, BIRD_G, BIRD_B = 255, 229, 76
local BIRD_WING_R, BIRD_WING_G, BIRD_WING_B = 245, 182, 45
local BEAK_R, BEAK_G, BEAK_B = 255, 136, 58
local EYE_R, EYE_G, EYE_B = 36, 32, 32
local TEXT_R, TEXT_G, TEXT_B = 20, 36, 54
local PANEL_R, PANEL_G, PANEL_B = 248, 251, 255
local PANEL_BORDER_R, PANEL_BORDER_G, PANEL_BORDER_B = 98, 132, 168
local DANGER_R, DANGER_G, DANGER_B = 214, 74, 74
local input_mode = "none"
local touch_handle = nil
local button_handle = nil
local button_active_level = 0
local button_last_level = 1
local audio_output = nil

local panel_handle, io_handle, width, height, panel_if = bm.get_display_lcd_params("display_lcd")
if not panel_handle then
    print("[lappybird] ERROR: get_display_lcd_params(display_lcd) failed: " .. tostring(io_handle))
    return
end

local ok, err = pcall(display.init, panel_handle, io_handle, width, height, panel_if)
if not ok then
    print("[lappybird] ERROR: init failed: " .. tostring(err))
    return
end

local screen_created = true

local function cleanup()
    if button_handle then
        pcall(button.off, button_handle)
        pcall(button.close, button_handle)
        button_handle = nil
    end

    if audio_output then
        pcall(audio.close, audio_output)
        audio_output = nil
    end

    if screen_created then
        pcall(display.end_frame)
        pcall(display.deinit)
        screen_created = false
    end
end

width = display.width()
height = display.height()

if width <= 0 or height <= 0 then
    print("[lappybird] ERROR: invalid display size after init")
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
local touch_consumed = false
local last_gap_top = nil

math.randomseed(os.time() + width * 13 + height * 17)

for i = 1, CLOUD_COUNT do
    cloud_offsets[i] = {
        x = ((i - 1) * width) // CLOUD_COUNT + math.random(0, 24),
        y = 12 + math.random(0, math.max(10, math.floor(play_height * 0.18))),
        size = 12 + math.random(0, 10),
        speed = 0.3 + math.random() * 0.4,
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
    local min_gap_top = PIPE_MARGIN
    local max_gap_top = play_bottom - PIPE_GAP - PIPE_MARGIN
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
        gap_bottom = gap_top + PIPE_GAP,
        scored = false,
    }
end

local function reset_round(next_state)
    score = 0
    bird_y = math.floor(play_height * 0.45)
    bird_vy = 0
    spawn_timer_ms = 0
    last_gap_top = nil
    pipes = {
        new_pipe(width + 48),
        new_pipe(width + 48 + math.floor(width * 0.56)),
    }
    state = next_state or "title"
end

local function flap()
    bird_vy = FLAP_VELOCITY
    if audio_output then
        pcall(audio.play_tone, audio_output, 920, 35)
    end
end

local function draw_cloud(x, y, size)
    display.fill_circle(x, y, size, CLOUD_R, CLOUD_G, CLOUD_B)
    display.fill_circle(x + size, y - 2, math.floor(size * 0.85), CLOUD_R, CLOUD_G, CLOUD_B)
    display.fill_circle(x + size * 2 - 2, y, math.floor(size * 0.72), CLOUD_R, CLOUD_G, CLOUD_B)
    display.fill_rect(x, y - math.floor(size * 0.5), size * 2, size, CLOUD_R, CLOUD_G, CLOUD_B)
end

local function draw_background()
    display.clear(SKY_R, SKY_G, SKY_B)
    display.fill_circle(width - 34, 28, 18, SUN_R, SUN_G, SUN_B)

    for i = 1, CLOUD_COUNT do
        local cloud = cloud_offsets[i]
        local drift = math.floor((frame_count * cloud.speed + cloud.x) % (width + 56)) - 28
        draw_cloud(drift, cloud.y, cloud.size)
    end

    display.fill_rect(0, play_bottom, width, GROUND_HEIGHT, GROUND_R, GROUND_G, GROUND_B)
    display.fill_rect(0, play_bottom + GROUND_HEIGHT - 8, width, 8, DIRT_R, DIRT_G, DIRT_B)

    local stripe_w = 14
    for x = 0, width + stripe_w, stripe_w * 2 do
        local offset = (frame_count * 2) % (stripe_w * 2)
        display.fill_rect(x - offset, play_bottom, stripe_w, 6, 234, 208, 108)
    end
end

local function draw_pipe(pipe)
    local x = math.floor(pipe.x)
    local top_h = pipe.gap_top
    local bottom_y = pipe.gap_bottom
    local bottom_h = play_bottom - bottom_y

    display.fill_rect(x, 0, PIPE_WIDTH, top_h, PIPE_R, PIPE_G, PIPE_B)
    display.fill_rect(x + PIPE_WIDTH - 7, 0, 7, top_h, PIPE_SHADE_R, PIPE_SHADE_G, PIPE_SHADE_B)
    display.fill_rect(x - 2, top_h - 10, PIPE_WIDTH + 4, 10, PIPE_CAP_R, PIPE_CAP_G, PIPE_CAP_B)

    display.fill_rect(x, bottom_y, PIPE_WIDTH, bottom_h, PIPE_R, PIPE_G, PIPE_B)
    display.fill_rect(x + PIPE_WIDTH - 7, bottom_y, 7, bottom_h, PIPE_SHADE_R, PIPE_SHADE_G, PIPE_SHADE_B)
    display.fill_rect(x - 2, bottom_y, PIPE_WIDTH + 4, 10, PIPE_CAP_R, PIPE_CAP_G, PIPE_CAP_B)
end

local function draw_bird()
    local bx = bird_x
    local by = math.floor(bird_y)
    local tilt = math.max(-8, math.min(8, math.floor(bird_vy)))
    local leg_y = by + BIRD_RADIUS + 2 + (tilt // 2)

    display.fill_circle(bx, by, BIRD_RADIUS, BIRD_R, BIRD_G, BIRD_B)
    display.fill_circle(bx - 2, by + 2, math.floor(BIRD_RADIUS * 0.65), BIRD_WING_R, BIRD_WING_G, BIRD_WING_B)
    display.fill_triangle(
        bx + BIRD_RADIUS - 1, by - 2,
        bx + BIRD_RADIUS + 10, by + 1,
        bx + BIRD_RADIUS - 1, by + 5,
        BEAK_R, BEAK_G, BEAK_B
    )
    display.fill_circle(bx + 3, by - 3, 3, 255, 255, 255)
    display.fill_circle(bx + 4, by - 3, 1, EYE_R, EYE_G, EYE_B)
    display.draw_line(bx - 6, by + BIRD_RADIUS - 2, bx - 2, leg_y, EYE_R, EYE_G, EYE_B)
    display.draw_line(bx + 1, by + BIRD_RADIUS - 2, bx + 5, leg_y, EYE_R, EYE_G, EYE_B)
end

local function draw_scoreboard()
    local box_w = 112
    local left_x = 8
    local right_x = width - box_w - 8

    display.fill_round_rect(left_x, 8, box_w, 40, 8, PANEL_R, PANEL_G, PANEL_B)
    display.draw_round_rect(left_x, 8, box_w, 40, 8, PANEL_BORDER_R, PANEL_BORDER_G, PANEL_BORDER_B)
    display.draw_text(left_x + 8, 18, "SCORE " .. tostring(score), {
        r = TEXT_R,
        g = TEXT_G,
        b = TEXT_B,
        font_size = 14,
        bg_r = PANEL_R,
        bg_g = PANEL_G,
        bg_b = PANEL_B,
    })

    display.fill_round_rect(right_x, 8, box_w, 40, 8, PANEL_R, PANEL_G, PANEL_B)
    display.draw_round_rect(right_x, 8, box_w, 40, 8, PANEL_BORDER_R, PANEL_BORDER_G, PANEL_BORDER_B)
    display.draw_text(right_x + 8, 18, "BEST " .. tostring(best_score), {
        r = TEXT_R,
        g = TEXT_G,
        b = TEXT_B,
        font_size = 14,
        bg_r = PANEL_R,
        bg_g = PANEL_G,
        bg_b = PANEL_B,
    })
end

local function draw_center_panel(title, subtitle, subtitle_color)
    local panel_w = math.min(width - 8, 232)
    local panel_h = 88
    local panel_x = (width - panel_w) // 2
    local panel_y = math.floor(play_height * 0.18)

    display.fill_round_rect(panel_x, panel_y, panel_w, panel_h, 12, PANEL_R, PANEL_G, PANEL_B)
    display.draw_round_rect(panel_x, panel_y, panel_w, panel_h, 12, PANEL_BORDER_R, PANEL_BORDER_G, PANEL_BORDER_B)
    display.draw_text_aligned(panel_x, panel_y + 8, panel_w, 24, title, {
        r = TEXT_R,
        g = TEXT_G,
        b = TEXT_B,
        font_size = 22,
        bg_r = PANEL_R,
        bg_g = PANEL_G,
        bg_b = PANEL_B,
        align = "center",
        valign = "middle",
    })
    display.draw_text_aligned(panel_x + 12, panel_y + 42, panel_w - 24, 18, subtitle, {
        r = subtitle_color.r,
        g = subtitle_color.g,
        b = subtitle_color.b,
        font_size = 14,
        bg_r = PANEL_R,
        bg_g = PANEL_G,
        bg_b = PANEL_B,
        align = "center",
        valign = "middle",
    })
end

local function action_label(verb)
    if input_mode == "lcd_touch" then
        return "tap to " .. verb
    end
    if input_mode == "button" then
        return "press button to " .. verb
    end
    return verb
end

local function render()
    draw_background()

    for i = 1, #pipes do
        draw_pipe(pipes[i])
    end

    draw_bird()
    draw_scoreboard()

    if state == "title" then
        draw_center_panel("Lappy Bird", action_label("start") .. " and " .. action_label("flap"), { r = 44, g = 86, b = 128 })
    elseif state == "crashed" then
        draw_center_panel("Crash", action_label("restart"), { r = DANGER_R, g = DANGER_G, b = DANGER_B })
    end

    display.present()
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
    if audio_output then
        pcall(audio.play_tone, audio_output, 180, 220)
    end
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
        pipe.x = pipe.x - PIPE_SPEED

        if not pipe.scored and pipe.x + PIPE_WIDTH < bird_x then
            pipe.scored = true
            score = score + 1
            if score > best_score then
                best_score = score
            end
            if audio_output then
                pcall(audio.play_tone, audio_output, 1320, 50)
            end
        end

        if pipe.x + PIPE_WIDTH < -4 then
            table.remove(pipes, i)
        elseif circle_rect_hit(bird_x, bird_y, BIRD_RADIUS, pipe.x, 0, PIPE_WIDTH, pipe.gap_top)
            or circle_rect_hit(bird_x, bird_y, BIRD_RADIUS, pipe.x, pipe.gap_bottom, PIPE_WIDTH, play_bottom - pipe.gap_bottom) then
            set_crashed()
        end
    end

    if bird_y - BIRD_RADIUS <= play_top or bird_y + BIRD_RADIUS >= play_bottom then
        set_crashed()
    end
end

local function init_input()
    if lcd_touch_ok then
        local touch_err
        touch_handle, touch_err = bm.get_lcd_touch_handle("lcd_touch")
        if touch_handle then
            ok, err = pcall(lcd_touch.sync, touch_handle)
            if ok then
                input_mode = "lcd_touch"
                return true
            end
            print("[lappybird] WARN: lcd_touch.sync failed: " .. tostring(err))
        else
            print("[lappybird] WARN: get_lcd_touch_handle(lcd_touch) failed: " .. tostring(touch_err))
        end
    else
        print("[lappybird] WARN: require(lcd_touch) failed")
    end

    if not button_ok then
        print("[lappybird] ERROR: require(button) failed")
        return false
    end

    local button_err
    button_handle, button_err = button.new(0, 0)
    if not button_handle then
        print("[lappybird] ERROR: button.new failed: " .. tostring(button_err))
        return false
    end

    local level, level_err = button.get_key_level(button_handle)
    if level == nil then
        print("[lappybird] ERROR: button.get_key_level failed: " .. tostring(level_err))
        cleanup()
        return false
    end
    button_last_level = level

    input_mode = "button"
    return true
end

local function consume_input_tap()
    if input_mode == "lcd_touch" then
        local polled, info = pcall(lcd_touch.poll, touch_handle)
        if not polled then
            print("[lappybird] ERROR: lcd_touch.poll failed: " .. tostring(info))
            return nil
        end

        local tapped = info.just_pressed and not touch_consumed
        touch_consumed = info.pressed
        return tapped
    end

    if input_mode == "button" then
        local level, level_err = button.get_key_level(button_handle)
        if level == nil then
            print("[lappybird] ERROR: button.get_key_level failed: " .. tostring(level_err))
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
        print("[lappybird] WARN: require(audio) failed")
        return
    end

    local output_codec, output_rate, output_channels, output_bits =
        bm.get_audio_codec_output_params("audio_dac")
    if not output_codec then
        print("[lappybird] WARN: get_audio_codec_output_params(audio_dac) failed: " .. tostring(output_rate))
        return
    end

    local output, out_err = audio.new_output(output_codec, output_rate, output_channels, output_bits)
    if not output then
        print("[lappybird] WARN: audio.new_output failed: " .. tostring(out_err))
        return
    end

    audio_output = output
    pcall(audio.set_volume, audio_output, SOUND_VOLUME)
end

if not init_input() then
    cleanup()
    return
end

init_audio()

reset_round("title")

display.begin_frame({ clear = true, r = SKY_R, g = SKY_G, b = SKY_B })

print(string.format("[lappybird] ready screen=%dx%d", width, height))
if input_mode == "lcd_touch" then
    print("[lappybird] lcd_touch ready, tap anywhere to flap, tap after crashing to restart")
else
    print("[lappybird] lcd_touch unavailable, using button to flap and restart")
end

for _ = 1, RUN_TIME_MS // FRAME_MS do
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

    render()
    frame_count = frame_count + 1
    delay.delay_ms(FRAME_MS)
end

cleanup()
print("[lappybird] done")
