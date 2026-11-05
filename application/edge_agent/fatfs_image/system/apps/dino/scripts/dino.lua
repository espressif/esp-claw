local display = require("display")
local screen, screen_info

local function center_text(x, y, w, h, text, options)
    local tw, th = screen:measure_text(text, options)
    screen:text(x + math.max(0, (w - tw) // 2), y + math.max(0, (h - th) // 2), text, options)
end
local dly = require("delay")
local sys = require("system")

local button_ok, button = pcall(require, "button")

local a = type(args) == "table" and args or {}

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local function num_arg(k, default)
    local v = a[k]
    if type(v) == "number" then return v end
    return default
end

local function str_arg(k, default)
    local v = a[k]
    if type(v) == "string" and v ~= "" then return v end
    return default
end

local function bool_arg(k, default)
    local v = a[k]
    if type(v) == "boolean" then return v end
    return default
end

local LOOK_PRESETS = {
    classic = { color = "gray", scale = 1.00 },
    chrome = { color = "gray", scale = 1.00 },
    mini_blue = { color = "blue", scale = 0.85 },
    big_orange = { color = "orange", scale = 1.25 },
}

local COLOR_PRESETS = {
    gray = {
        body = { r = 83, g = 83, b = 83 },
        dark = { r = 55, g = 55, b = 55 },
        highlight = { r = 120, g = 120, b = 120 },
        belly = { r = 83, g = 83, b = 83 },
        outline = { r = 45, g = 45, b = 45 },
    },
    green = {
        body = { r = 55, g = 185, b = 80 },
        dark = { r = 25, g = 105, b = 55 },
        highlight = { r = 145, g = 235, b = 135 },
        belly = { r = 225, g = 245, b = 170 },
        outline = { r = 25, g = 65, b = 35 },
    },
    red = {
        body = { r = 220, g = 70, b = 50 },
        dark = { r = 150, g = 30, b = 25 },
        highlight = { r = 250, g = 160, b = 120 },
        belly = { r = 250, g = 210, b = 180 },
        outline = { r = 90, g = 20, b = 15 },
    },
    blue = {
        body = { r = 70, g = 145, b = 235 },
        dark = { r = 35, g = 75, b = 160 },
        highlight = { r = 160, g = 215, b = 255 },
        belly = { r = 220, g = 240, b = 255 },
        outline = { r = 20, g = 45, b = 100 },
    },
    purple = {
        body = { r = 150, g = 95, b = 220 },
        dark = { r = 90, g = 50, b = 145 },
        highlight = { r = 215, g = 175, b = 255 },
        belly = { r = 245, g = 225, b = 255 },
        outline = { r = 60, g = 35, b = 105 },
    },
    orange = {
        body = { r = 245, g = 135, b = 35 },
        dark = { r = 165, g = 75, b = 20 },
        highlight = { r = 255, g = 205, b = 115 },
        belly = { r = 255, g = 235, b = 175 },
        outline = { r = 95, g = 45, b = 15 },
    },
}

local function copy_color(c)
    return { r = c.r, g = c.g, b = c.b }
end

local function rgb(v, fallback)
    if type(v) == "table" and type(v.r) == "number" and type(v.g) == "number" and type(v.b) == "number" then
        return {
            r = clamp(math.floor(v.r), 0, 255),
            g = clamp(math.floor(v.g), 0, 255),
            b = clamp(math.floor(v.b), 0, 255),
        }
    end
    return copy_color(fallback)
end

local look = LOOK_PRESETS[str_arg("preset", "classic")] or LOOK_PRESETS.classic
local RUN_MS = math.floor(num_arg("run_ms", 0))
local FRAME_MS = clamp(math.floor(num_arg("frame_ms", 33)), 16, 1000)
local SCALE = clamp(num_arg("scale", look.scale), 0.70, 1.60)
local COLOR_NAME = str_arg("color", look.color)
local palette_base = COLOR_PRESETS[COLOR_NAME] or COLOR_PRESETS.gray
local PAL = {
    body = rgb(a.body_color, palette_base.body),
    dark = rgb(a.dark_color, palette_base.dark),
    highlight = rgb(a.highlight_color, palette_base.highlight),
    belly = rgb(a.belly_color, palette_base.belly),
    outline = rgb(a.outline_color, palette_base.outline),
}

local GRAVITY = num_arg("gravity", 0.42)
local JUMP_V = num_arg("jump_v", -10.0)
local SCROLL_V0 = num_arg("scroll_v0", 2.1)
local SCROLL_MAX = num_arg("scroll_max", 3.4)
local SCROLL_ACC = num_arg("scroll_acc", 0.00030)
local SPAWN_MIN = math.floor(num_arg("spawn_min", 175))
local SPAWN_MAX = math.floor(num_arg("spawn_max", 300))
local FONT_SIZE_ARG = a.font_size
local INPUT_MODE = str_arg("input_mode", "auto")
local BUTTON_GPIO = a.button_gpio
if type(BUTTON_GPIO) ~= "number" then BUTTON_GPIO = a.key_gpio end
if type(BUTTON_GPIO) == "number" then BUTTON_GPIO = math.floor(BUTTON_GPIO) end
local BUTTON_ACTIVE_LEVEL = math.floor(num_arg("button_active_level", num_arg("key_active_level", 0)))
local BUTTON_LONG_PRESS_MS = math.floor(num_arg("button_long_press_ms", 1500))
local BUTTON_SHORT_PRESS_MS = math.floor(num_arg("button_short_press_ms", 180))
local ENABLE_TOUCH = bool_arg("enable_touch", INPUT_MODE ~= "button")
local ENABLE_BUTTON = bool_arg("enable_button", INPUT_MODE == "button" or INPUT_MODE == "both" or BUTTON_GPIO ~= nil)

local display_ok, display_info = pcall(display.open)
if not display_ok then
    print("[dino] ERROR: display init failed: " .. tostring(display_info))
    return
end
screen = display_info
screen_info = screen:info()

local ready = true
local W, H = screen_info.width, screen_info.height
if W <= 0 or H <= 0 then
    print("[dino] ERROR: invalid display size after init")
    pcall(screen.close, screen)
    return
end
local DEFAULT_FZ = clamp(math.floor(H / 12), 14, 24)
local FZ = clamp(math.floor(type(FONT_SIZE_ARG) == "number" and FONT_SIZE_ARG or DEFAULT_FZ), 12, 40)

local touch_enabled, button_handle = false, nil
local touch_down, button_last_level = false, nil
local input_sources = {}

local function cleanup()
    if button_handle then
        pcall(button.off, button_handle)
        pcall(button.close, button_handle)
        button_handle = nil
    end
    if ready then
        pcall(screen.close, screen)
        ready = false
    end
end

local function add_input_source(name)
    input_sources[#input_sources + 1] = name
end

local function init_touch_input()
    if not ENABLE_TOUCH or not screen_info.touch_available then return false end
    touch_enabled = true
    add_input_source("touch:display")
    return true
end

local function init_button_input()
    if not ENABLE_BUTTON and BUTTON_GPIO == nil then return false end
    if not button_ok then
        print("[dino] WARN: require(button) failed")
        return false
    end

    local handle, err = button.new(BUTTON_GPIO or 0, BUTTON_ACTIVE_LEVEL, BUTTON_LONG_PRESS_MS, BUTTON_SHORT_PRESS_MS)
    if not handle then
        print("[dino] WARN: button.new gpio=" .. tostring(BUTTON_GPIO or 0) .. " failed: " .. tostring(err))
        return false
    end

    button_handle = handle
    local level, level_err = button.get_key_level(button_handle)
    if level == nil then
        print("[dino] WARN: button.get_key_level failed: " .. tostring(level_err))
        pcall(button.close, button_handle)
        button_handle = nil
        return false
    end

    button_last_level = level
    add_input_source("button:gpio" .. tostring(BUTTON_GPIO or 0))
    return true
end

local function init_input()
    init_touch_input()
    init_button_input()

    if #input_sources == 0 then
        print("[dino] ERROR: no input source available; enable touch or pass button_gpio")
        cleanup()
        return false
    end

    return true
end

math.randomseed(math.floor(sys.millis()) & 0x7fffffff)

local function rnd(lo, hi)
    return math.random(lo, hi)
end

local BASE_W, BASE_H = 48, 44
local DW = math.floor(BASE_W * SCALE + 0.5)
local DH = math.floor(BASE_H * SCALE + 0.5)
local DX = math.floor(num_arg("dino_x", 34))
local SEA_H = 24
local TOP_SAFE = math.floor(num_arg("top_safe", clamp(math.floor(H * 0.12), 18, 30)))
local GROUND_MARGIN = math.floor(num_arg("ground_margin", clamp(math.floor(H * 0.11), 22, 34)))
local GY = H - GROUND_MARGIN
local GH = H - GY

local dino_y, dino_vy, on_ground
local obs, gulls, palms, decor, bubbles = {}, {}, {}, {}, {}
local scroll_px, next_spawn, speed, anim_t
local score, best, over = 0, 0, false

local THEME = {
    sky = { r = 247, g = 244, b = 235 },
    sun = { r = 255, g = 196, b = 82 },
    dune_far = { r = 235, g = 226, b = 206 },
    dune_near = { r = 222, g = 207, b = 176 },
    sand = { r = 246, g = 231, b = 199 },
    sand_mark = { r = 216, g = 191, b = 145 },
    ink = { r = 35, g = 47, b = 43 },
    muted = { r = 112, g = 119, b = 105 },
    cactus = { r = 45, g = 116, b = 82 },
    cactus_light = { r = 86, g = 151, b = 104 },
    card = { r = 255, g = 252, b = 244 },
    accent = { r = 229, g = 91, b = 67 },
}
local color_cache = {}

local function frame_color(r, g, b)
    local key = (r << 16) | (g << 8) | b
    local color = color_cache[key]
    if color == nil then
        color = display.color(r, g, b)
        color_cache[key] = color
    end
    return color
end
local function fr(x, y, w, h, r, g, b) screen:fill_rect(x, y, w, h, frame_color(r, g, b)) end
local function dr(x, y, w, h, r, g, b) screen:stroke_rect(x, y, w, h, frame_color(r, g, b)) end
local function fc(x, y, r, cr, cg, cb) screen:fill_circle(x, y, r, frame_color(cr, cg, cb)) end
local function frr(x, y, w, h, rd, r, g, b) screen:fill_round_rect(x, y, w, h, rd, frame_color(r, g, b)) end
local function ft(x1, y1, x2, y2, x3, y3, r, g, b) screen:fill_triangle(x1, y1, x2, y2, x3, y3, frame_color(r, g, b)) end
local function ln(x1, y1, x2, y2, r, g, b) screen:line(x1, y1, x2, y2, frame_color(r, g, b)) end
local function col(c) return c.r, c.g, c.b end

local function init_decor()
    gulls, palms, decor, bubbles = {}, {}, {}, {}
    for i = 1, 4 do gulls[i] = { x = rnd(0, W), y = rnd(24, 78), s = rnd(10, 18), v = (18 + rnd(0, 18)) / 100 } end
    for i = 1, 8 do decor[i] = { x = rnd(0, W * 2), t = rnd(0, 2) } end
end

local function reset()
    dino_y, dino_vy, on_ground = GY - DH, 0, true
    obs = {}
    scroll_px, next_spawn, speed, anim_t = 0, 160, SCROLL_V0, 0
    score, over = 0, false
end

local function consume_touch_press()
    local down = #screen:touch().points > 0
    local pressed = down and not touch_down
    touch_down = down
    return pressed
end

local function consume_press()
    local pressed = false

    if touch_enabled then
        pressed = consume_touch_press()
    end

    if button_handle then
        local level, level_err = button.get_key_level(button_handle)
        if level == nil then
            print("[dino] ERROR: button.get_key_level failed: " .. tostring(level_err))
            return nil
        end

        pressed = pressed or (level == BUTTON_ACTIVE_LEVEL and button_last_level ~= BUTTON_ACTIVE_LEVEL)
        button_last_level = level
    end

    return pressed
end

local function input()
    local pressed = consume_press()
    if pressed == nil then return end
    if over then
        if pressed then return "restart" end
        return
    end
    if pressed and on_ground then
        dino_vy = JUMP_V
        on_ground = false
    end
end

local function spawn_obstacle()
    local kind = rnd(1, 3)
    if kind == 1 then
        local w, h = rnd(14, 18), rnd(28, 42)
        obs[#obs + 1] = { x = W + 6, y = GY - h, w = w, h = h, kind = "cactus", scored = false, seed = rnd(0, 6) }
    elseif kind == 2 then
        local w, h = rnd(24, 34), rnd(24, 38)
        obs[#obs + 1] = { x = W + 6, y = GY - h, w = w, h = h, kind = "cactus_pair", scored = false, seed = rnd(0, 5) }
    else
        local w, h = rnd(38, 48), rnd(20, 30)
        obs[#obs + 1] = { x = W + 6, y = GY - h, w = w, h = h, kind = "cactus_cluster", scored = false, seed = rnd(0, 5) }
    end
end

local function step(df)
    anim_t = anim_t + df
    if over then return end

    speed = clamp(speed + SCROLL_ACC * df, SCROLL_V0, SCROLL_MAX)
    if not on_ground then
        dino_vy = dino_vy + GRAVITY * df
        dino_y = dino_y + dino_vy * df
        if dino_y >= GY - DH then
            dino_y, dino_vy, on_ground = GY - DH, 0, true
        end
    end

    local sv = speed * df
    scroll_px = scroll_px + sv
    for i = #obs, 1, -1 do
        local o = obs[i]
        o.x = o.x - sv
        if (not o.scored) and (o.x + o.w) < DX then
            o.scored = true
            score = score + 1
            if score > best then best = score end
        end
        if o.x + o.w < -24 then table.remove(obs, i) end
    end

    next_spawn = next_spawn - sv
    if next_spawn <= 0 then
        spawn_obstacle()
        next_spawn = rnd(SPAWN_MIN, SPAWN_MAX)
    end

    for _, g in ipairs(gulls) do
        g.x = g.x - g.v * df
        if g.x < -12 then
            g.x, g.y, g.s = W + rnd(0, 80), rnd(24, 78), rnd(10, 18)
        end
    end
    for _, p in ipairs(palms) do
        p.x = p.x - 0.55 * df
        if p.x < -30 then p.x, p.h = W + rnd(10, 140), rnd(30, 50) end
    end
    for _, d in ipairs(decor) do
        d.x = d.x - sv * 0.85
        if d.x < -12 then d.x, d.t = W + rnd(30, 200), rnd(0, 2) end
    end
    for _, bubble in ipairs(bubbles) do
        bubble.y = bubble.y - bubble.vy * df
        if bubble.y < GY - SEA_H - 2 then bubble.y, bubble.x, bubble.r = GY - 1, rnd(0, W), rnd(1, 3) end
    end

    local dx1, dy1 = DX + math.floor(DW * 0.18), dino_y + math.floor(DH * 0.20)
    local dx2, dy2 = DX + DW - math.floor(DW * 0.18), dino_y + DH - math.floor(DH * 0.12)
    for i = 1, #obs do
        local o = obs[i]
        if dx1 < o.x + o.w - 2 and dx2 > o.x + 2 and dy1 < o.y + o.h and dy2 > o.y + 2 then
            over = true
            break
        end
    end
end

local function draw_bg()
    fc(W - 48, TOP_SAFE + 50, 25, col(THEME.sun))
    fc(W - 48, TOP_SAFE + 50, 17, 255, 215, 126)

    local far_y = GY - 50
    ft(0, GY, math.floor(W * 0.27), far_y, math.floor(W * 0.58), GY, col(THEME.dune_far))
    ft(math.floor(W * 0.38), GY, math.floor(W * 0.72), far_y - 8, W, GY, col(THEME.dune_far))
    ft(0, GY, math.floor(W * 0.42), GY - 28, math.floor(W * 0.77), GY, col(THEME.dune_near))
    ft(math.floor(W * 0.62), GY, math.floor(W * 0.86), GY - 30, W, GY, col(THEME.dune_near))

    for _, g in ipairs(gulls) do
        local x, y, s = math.floor(g.x), math.floor(g.y), g.s
        ln(x, y + 8, x + 4, y + 4, col(THEME.muted))
        ln(x + 4, y + 4, x + 8, y + 4, col(THEME.muted))
        ln(x + 8, y + 4, x + 12, y, col(THEME.muted))
        ln(x + 12, y, x + s, y, col(THEME.muted))
        ln(x + s, y, x + s + 6, y + 5, col(THEME.muted))
        ln(x + s + 6, y + 5, x + s + 12, y + 5, col(THEME.muted))
        ln(x + s + 12, y + 5, x + s + 16, y + 8, col(THEME.muted))
    end
end

local function draw_ground()
    fr(0, GY, W, GH, col(THEME.sand))
    fr(0, GY, W, 2, col(THEME.ink))
    fr(0, GY + 8, W, 1, 236, 214, 174)
    for _, d in ipairs(decor) do
        local x = math.floor(d.x)
        local y = GY + 9 + (d.t * 4)
        if d.t == 0 then
            fr(x, y, 8, 1, col(THEME.sand_mark))
        elseif d.t == 1 then
            fr(x, y, 3, 1, col(THEME.sand_mark))
            fr(x + 6, y, 6, 1, col(THEME.sand_mark))
        else
            fr(x, y, 2, 2, col(THEME.sand_mark))
        end
    end
    local off = math.floor(scroll_px) % 28
    for x = -off, W, 28 do fr(x, GY + 18, 10, 1, col(THEME.sand_mark)) end
end

local function make_scaled_draw(x, y)
    local function sx(v) return x + math.floor(v * SCALE + 0.5) end
    local function sy(v) return y + math.floor(v * SCALE + 0.5) end
    local function sz(v) return math.max(1, math.floor(v * SCALE + 0.5)) end
    return {
        rect = function(px, py, pw, ph, c) fr(sx(px), sy(py), sz(pw), sz(ph), col(c)) end,
        round = function(px, py, pw, ph, rd, c) frr(sx(px), sy(py), sz(pw), sz(ph), sz(rd), col(c)) end,
        circle = function(px, py, pr, c) fc(sx(px), sy(py), sz(pr), col(c)) end,
        tri = function(x1, y1, x2, y2, x3, y3, c) ft(sx(x1), sy(y1), sx(x2), sy(y2), sx(x3), sy(y3), col(c)) end,
        line = function(x1, y1, x2, y2, c) ln(sx(x1), sy(y1), sx(x2), sy(y2), col(c)) end,
    }
end

local function draw_dino(x, y)
    local s = make_scaled_draw(x, y)
    local leg = on_ground and (math.floor(anim_t / 4) % 2) or 2
    local body, cut = PAL.body, THEME.sky

    local function block(px, py, pw, ph)
        s.rect(px, py, pw, ph, body)
    end

    local function erase(px, py, pw, ph)
        s.rect(px, py, pw, ph, cut)
    end

    -- Tail: stepped blocks keep the silhouette pixelated while staying editable.
    block(0, 18, 3, 11)
    block(3, 22, 5, 8)
    block(8, 25, 6, 7)
    block(14, 28, 6, 6)

    -- Body and chest.
    block(10, 23, 20, 11)
    block(15, 18, 17, 12)
    block(20, 14, 12, 10)
    block(24, 11, 8, 7)

    -- Neck and head.
    block(28, 4, 8, 19)
    block(31, 0, 18, 12)
    block(34, 12, 15, 7)
    block(40, 19, 8, 2)

    -- Chrome-Dino style negative pixels: eye and mouth notch.
    erase(34, 4, 3, 3)
    erase(41, 12, 8, 4)

    -- Small arm.
    block(31, 24, 8, 3)
    block(38, 27, 3, 4)

    -- Belly/hip mass.
    block(18, 32, 14, 4)

    -- Running legs. Each frame is still procedural parts, not a sprite bitmap.
    if leg == 0 then
        block(14, 35, 5, 8)
        block(11, 42, 8, 2)
        block(27, 35, 4, 6)
        block(27, 40, 7, 2)
    elseif leg == 1 then
        block(14, 35, 4, 6)
        block(14, 40, 7, 2)
        block(27, 35, 5, 8)
        block(27, 42, 8, 2)
    else
        block(15, 35, 4, 7)
        block(27, 35, 4, 7)
    end
end

local function draw_shadow(cx)
    if on_ground then
        fr(cx - math.floor(DW * 0.42), GY + 3, math.floor(DW * 0.84), 2, 190, 166, 126)
    else
        fr(cx - math.floor(DW * 0.28), GY + 4, math.floor(DW * 0.56), 1, 224, 205, 170)
    end
end

local function draw_cactus_part(x, base, h, w)
    fr(x, base - h, w, h, col(THEME.cactus))
    fr(x + 1, base - h + 2, math.min(2, w - 1), h - 4, col(THEME.cactus_light))
    fr(x - 5, base - math.floor(h * 0.58), 5, 4, col(THEME.cactus))
    fr(x - 7, base - math.floor(h * 0.72), 3, 10, col(THEME.cactus))
    fr(x + w, base - math.floor(h * 0.42), 5, 4, col(THEME.cactus))
    fr(x + w + 3, base - math.floor(h * 0.58), 3, 9, col(THEME.cactus))
end

local function draw_cactus(o)
    local x, y, w, h = math.floor(o.x), o.y, o.w, o.h
    local base = y + h
    if o.kind == "cactus_pair" then
        draw_cactus_part(x + 2, base, h, 5)
        draw_cactus_part(x + w - 9, base, math.max(18, h - 7), 5)
    elseif o.kind == "cactus_cluster" then
        draw_cactus_part(x + 4, base, h, 5)
        draw_cactus_part(x + 17, base, math.max(16, h - 8), 5)
        draw_cactus_part(x + 30, base, math.max(18, h - 3), 5)
    else
        draw_cactus_part(x + (w // 2) - 3, base, h, 6)
    end
end

local function draw_obstacles()
    for i = 1, #obs do
        draw_cactus(obs[i])
    end
end

local TXT = { color = frame_color(col(THEME.ink)), font_size = FZ }
local TXT_C = {
    color = frame_color(col(THEME.ink)),
    font_size = FZ,
}

local function draw_hud()
    local y = TOP_SAFE
    local left_w = math.min(120, math.floor(W * 0.34))
    local right_w = math.min(110, math.floor(W * 0.31))
    frr(10, y, left_w, FZ + 19, 9, col(THEME.card))
    screen:stroke_round_rect(10, y, left_w, FZ + 19, 9, frame_color(222, 213, 195))
    screen:text(19, y + 4, "BEST", { color = frame_color(col(THEME.muted)), font_size = math.max(10, FZ - 7) })
    screen:text(19, y + 13, string.format("%05d", best), { color = frame_color(col(THEME.ink)), font_size = FZ })

    local rx = W - right_w - 10
    frr(rx, y, right_w, FZ + 19, 9, col(THEME.ink))
    screen:text(rx + 10, y + 4, "RUN", { color = frame_color(191, 205, 178), font_size = math.max(10, FZ - 7) })
    screen:text(rx + 10, y + 13, string.format("%05d", score), { color = frame_color(col(THEME.card)), font_size = FZ })
end

local function overlay_center(lines)
    local lh = FZ + 8
    local max_w = 0
    for _, text in ipairs(lines) do
        local tw = screen:measure_text(text, { font_size = FZ })
        if tw > max_w then max_w = tw end
    end
    local bw = math.min(W - 10, max_w + 40)
    local bh = #lines * lh + 12
    local bx = (W - bw) // 2
    local by = (H - bh) // 2
    frr(bx + 4, by + 5, bw, bh, 13, 205, 190, 163)
    frr(bx, by, bw, bh, 13, col(THEME.card))
    screen:stroke_round_rect(bx, by, bw, bh, 13, frame_color(218, 205, 181))
    for i, text in ipairs(lines) do
        center_text(bx, by + 6 + (i - 1) * lh, bw, FZ, text, TXT_C)
    end
end

local function draw_title_banner()
    overlay_center({ "DINO DASH", "TAP OR PRESS TO JUMP" })
end

local function draw_game_over()
    overlay_center({ "RUN COMPLETE", string.format("SCORE  %05d", score), "TAP OR PRESS TO RETRY" })
end

local function render()
    screen:begin({ clear = frame_color(col(THEME.sky)) })
    draw_bg()
    draw_ground()
    draw_obstacles()
    draw_shadow(DX + DW // 2)
    draw_dino(DX, math.floor(dino_y))
    draw_hud()
    if score == 0 and on_ground and #obs == 0 then draw_title_banner() end
    if over then
        draw_game_over()
    end
    screen:present()
end

if not init_input() then return end
init_decor()
reset()
print(string.format(
    "[dino] ready %dx%d input=%s preset=%s color=%s scale=%.2f run_ms=%d",
    W, H, table.concat(input_sources, "+"), str_arg("preset", "classic"), COLOR_NAME, SCALE, RUN_MS
))

local t0, last = sys.millis(), sys.millis()
local ok2, err2 = xpcall(function()
    while true do
        if RUN_MS > 0 and (sys.millis() - t0) >= RUN_MS then break end
        local now = sys.millis()
        local dt = now - last
        if dt < 0 then dt = 0 end
        last = now
        local df = dt / FRAME_MS
        if df > 3 then df = 3 end
        if input() == "restart" then reset() end
        step(df)
        render()
        local sl = FRAME_MS - (sys.millis() - now)
        if sl > 0 then dly.delay_ms(sl) end
    end
end, debug.traceback)

cleanup()
if not ok2 then print("[dino] ERROR: " .. tostring(err2)) end
print("[dino] done")
