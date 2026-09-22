local display = require("display")
local delay = require("delay")
local system = require("system")
local audio_ok, audio = pcall(require, "audio")

local script_dir = assert(debug.getinfo(1, "S").source:match("^@(.*/)"), "script directory unavailable")
local app_dir = assert(script_dir:match("^(.*)/scripts/$"), "app directory unavailable")
local app_args = type(args) == "table" and args or {}

local function int_arg(name, default, minimum, maximum)
    local value = type(app_args[name]) == "number" and math.floor(app_args[name]) or default
    return math.max(minimum, math.min(maximum, value))
end

local FRAME_MS = int_arg("frame_ms", 33, 16, 200)
local RUN_MS = int_arg("run_ms", 0, 0, 3600000)
local FRAMEBUFFER_COUNT = int_arg("framebuffer_count", 1, 1, 2)
local ENEMY_COUNT = int_arg("enemy_count", 14, 8, 24)
local PARTICLE_COUNT = int_arg("particle_count", 72, 32, 112)
local SOUND_VOLUME = int_arg("sound_volume", 72, 0, 100)
local DEBUG_OVERLAY = app_args.debug == true

local screen
local fonts = {}
local opened = false
local audio_output, audio_player
local sfx_pending, sfx_current_priority

local SFX = {
    start = { path = app_dir .. "/assets/sfx/start.aac", priority = 3 },
    destroy = { path = app_dir .. "/assets/sfx/destroy.aac", priority = 2 },
    elite_destroy = { path = app_dir .. "/assets/sfx/elite_destroy.aac", priority = 4 },
    damage = { path = app_dir .. "/assets/sfx/damage.aac", priority = 6 },
    pickup = { path = app_dir .. "/assets/sfx/pickup.aac", priority = 5 },
    wave = { path = app_dir .. "/assets/sfx/wave.aac", priority = 5 },
    game_over = { path = app_dir .. "/assets/sfx/game_over.aac", priority = 10 },
}

local function request_sfx(name)
    if not audio_player then return end
    local effect = SFX[name]
    if effect and (not sfx_pending or effect.priority >= sfx_pending.priority) then sfx_pending = effect end
end

local function drain_sfx()
    if not audio_player or not sfx_pending then return end
    local poll_ok, state = pcall(audio_player.poll, audio_player)
    if not poll_ok then
        print("[neon_strike] WARN: audio poll failed: " .. tostring(state))
        sfx_pending = nil
        return
    end
    if state.running then
        if sfx_pending.priority <= (sfx_current_priority or 0) then
            sfx_pending = nil
            return
        end
        local stop_ok, stopped, stop_err = pcall(audio_player.stop, audio_player)
        if not stop_ok or not stopped then
            print("[neon_strike] WARN: audio stop failed: " .. tostring(stop_ok and stop_err or stopped))
            sfx_pending = nil
        end
        sfx_current_priority = nil
        return
    end

    local effect = sfx_pending
    local play_ok, played, play_err = pcall(audio_player.play, audio_player, effect.path)
    if play_ok and played then
        sfx_current_priority = effect.priority
    else
        print("[neon_strike] WARN: audio play failed: " .. tostring(play_ok and play_err or played))
    end
    sfx_pending = nil
end

local function init_audio()
    if SOUND_VOLUME == 0 then return end
    if not audio_ok then
        print("[neon_strike] WARN: audio module unavailable: " .. tostring(audio))
        return
    end
    local output_ok, output, output_err = pcall(audio.open_output)
    if not output_ok or not output then
        print("[neon_strike] WARN: audio output unavailable: " .. tostring(output_ok and output_err or output))
        return
    end
    local volume_ok, volume_set, volume_err = pcall(output.set_volume, output, SOUND_VOLUME)
    if not volume_ok or not volume_set then
        print("[neon_strike] WARN: audio volume setup failed: " .. tostring(volume_ok and volume_err or volume_set))
    end
    local player_ok, player, player_err = pcall(audio.player, { output = output })
    if not player_ok or not player then
        print("[neon_strike] WARN: audio player unavailable: " .. tostring(player_ok and player_err or player))
        pcall(output.close, output)
        return
    end
    audio_output, audio_player = output, player
end

local function cleanup()
    if audio_player then pcall(audio_player.close, audio_player) end
    if audio_output then pcall(audio_output.close, audio_output) end
    audio_player, audio_output = nil, nil
    if fonts.title then pcall(fonts.title.close, fonts.title) end
    if fonts.body then pcall(fonts.body.close, fonts.body) end
    fonts.title, fonts.body = nil, nil
    if opened then pcall(screen.close, screen) end
    screen, opened = nil, false
end

local function open_resources()
    screen = display.open({ framebuffer_count = FRAMEBUFFER_COUNT })
    opened = true
    fonts.body = display.load_font(app_dir .. "/assets/game_body_12.dfn")
    fonts.title = display.load_font(app_dir .. "/assets/game_title_18.dfn")
end

local open_ok, open_err = xpcall(open_resources, debug.traceback)
if not open_ok then
    cleanup()
    print("[neon_strike] ERROR: open failed: " .. tostring(open_err))
    return
end

init_audio()

local info = screen:info()
local width, height = info.width, info.height
if width < 240 or height < 240 then
    cleanup()
    print("[neon_strike] ERROR: screen is too small")
    return
end

local COLORS = {
    sky_1 = display.color(4, 6, 17),
    sky_2 = display.color(7, 10, 26),
    sky_3 = display.color(10, 15, 37),
    sky_4 = display.color(13, 20, 45),
    sky_5 = display.color(16, 24, 49),
    nebula = display.color(76, 52, 156, 66),
    grid = display.color(44, 67, 105, 92),
    star_far = display.color(58, 76, 112),
    star_mid = display.color(113, 151, 196),
    star_near = display.color(222, 244, 255),
    planet_outer = display.color(41, 52, 92),
    planet_inner = display.color(16, 25, 55),
    planet_light = display.color(74, 102, 155),
    cyan = display.color(51, 225, 245),
    cyan_soft = display.color(86, 221, 255, 74),
    cyan_dark = display.color(11, 91, 133),
    white = display.color(239, 249, 255),
    navy = display.color(8, 19, 44),
    amber = display.color(255, 190, 70),
    amber_hot = display.color(255, 239, 158),
    orange = display.color(255, 105, 54),
    pink = display.color(255, 68, 133),
    pink_soft = display.color(255, 92, 160, 64),
    violet = display.color(159, 98, 255),
    violet_soft = display.color(159, 98, 255, 58),
    red = display.color(255, 72, 82),
    red_soft = display.color(255, 48, 70, 58),
    green = display.color(76, 230, 153),
    smoke = display.color(91, 108, 142, 92),
    smoke_dim = display.color(65, 76, 110, 48),
    panel = display.color(6, 10, 24, 226),
    panel_line = display.color(75, 100, 145, 150),
    text = display.color(227, 240, 255),
    muted = display.color(116, 140, 176),
    black_overlay = display.color(1, 3, 10, 196),
}

local BODY_TEXT = { font = fonts.body, color = COLORS.text }
local BODY_MUTED = { font = fonts.body, color = COLORS.muted }
local BODY_CYAN = { font = fonts.body, color = COLORS.cyan }
local BODY_AMBER = { font = fonts.body, color = COLORS.amber }
local TITLE_TEXT = { font = fonts.title, color = COLORS.white }
local TITLE_CYAN = { font = fonts.title, color = COLORS.cyan }
local TITLE_RED = { font = fonts.title, color = COLORS.red }
local SKY_BANDS = { COLORS.sky_1, COLORS.sky_2, COLORS.sky_3, COLORS.sky_4, COLORS.sky_5 }

local HUD_HEIGHT = 36
local BODY_WIDTH, TITLE_WIDTH = 6, 9
local PLAYER_BULLET_COUNT = 18
local ENEMY_BULLET_COUNT = 16
local PICKUP_COUNT = 3
local SHOCKWAVE_COUNT = 6

math.randomseed((system.millis() + width * 31 + height * 17) & 0x7fffffff)

local function clamp(value, minimum, maximum)
    return math.max(minimum, math.min(maximum, value))
end

local function pixel(value)
    return math.floor(value + 0.5)
end

-- Fixed pools keep frame latency stable during dense combat.
local stars, enemies, bullets, enemy_bullets = {}, {}, {}, {}
local particles, shockwaves, pickups = {}, {}, {}

for i = 1, 54 do
    local layer = ((i - 1) % 3) + 1
    stars[i] = {
        x = math.random(0, width - 1),
        y = math.random(HUD_HEIGHT, height - 1),
        layer = layer,
        speed = layer == 1 and math.random(10, 18) or layer == 2 and math.random(28, 42) or math.random(62, 88),
    }
end
for i = 1, ENEMY_COUNT do enemies[i] = { active = false } end
for i = 1, PLAYER_BULLET_COUNT do bullets[i] = { active = false, x = 0, y = 0, vx = 0, vy = 0 } end
for i = 1, ENEMY_BULLET_COUNT do enemy_bullets[i] = { active = false, x = 0, y = 0, vx = 0, vy = 0 } end
for i = 1, PARTICLE_COUNT do particles[i] = { active = false, x = 0, y = 0, vx = 0, vy = 0, life = 0, max_life = 0, kind = 1 } end
for i = 1, SHOCKWAVE_COUNT do shockwaves[i] = { active = false, x = 0, y = 0, radius = 0, speed = 0, life = 0, kind = 1 } end
for i = 1, PICKUP_COUNT do pickups[i] = { active = false, x = 0, y = 0, phase = 0 } end

local player = {
    x = width * 0.5,
    y = height * 0.78,
    target_x = width * 0.5,
    target_y = height * 0.78,
    bank = 0,
    invulnerable = 0,
}

local score, health, kills, wave = 0, 100, 0, 1
local combo, combo_timer, shot_timer, overdrive = 1, 0, 0, 0
local intro_timer, wave_banner, damage_flash, shake_timer = 2.2, 1.8, 0, 0
local game_state, state_timer, spawn_serial = "playing", 0, 0
local perf = { fps = 0, draw_ms = 0, present_us = 0, frames = 0, window_ms = system.millis() }
local hud = { score = "SCORE 000000", wave = "WAVE 01", combo = "", status = "", debug = "" }

local function enemy_limit()
    return math.min(ENEMY_COUNT, 6 + wave * 2)
end

local function configure_enemy(enemy, initial)
    spawn_serial = spawn_serial + 1
    local roll = math.random(1, 100)
    local kind = 1
    if wave >= 3 and spawn_serial % 17 == 0 then kind = 4
    elseif wave >= 2 and roll > 72 then kind = 3
    elseif roll > 40 then kind = 2 end

    enemy.kind = kind
    enemy.radius = kind == 1 and 8 or kind == 2 and 10 or kind == 3 and 14 or 18
    enemy.max_hp = kind == 1 and 1 or kind == 2 and 2 or kind == 3 and 4 or 8
    enemy.hp = enemy.max_hp
    enemy.x = math.random(enemy.radius + 6, math.max(enemy.radius + 6, width - enemy.radius - 7))
    enemy.y = initial and math.random(-height, -20) or math.random(-150, -28)
    enemy.vx = math.random(-20, 20)
    enemy.vy = (kind == 1 and math.random(62, 88) or kind == 2 and math.random(46, 66) or kind == 3 and math.random(30, 44) or 28) + wave * 2
    enemy.phase = math.random() * math.pi * 2
    enemy.fire_timer = 0.8 + math.random() * 1.8
    enemy.active = true
end

local function sync_enemies(initial)
    local limit = enemy_limit()
    for index, enemy in ipairs(enemies) do
        if index <= limit and (initial or not enemy.active) then configure_enemy(enemy, initial) end
        if index > limit then enemy.active = false end
    end
end

local function clear_pool(pool)
    for _, item in ipairs(pool) do item.active = false end
end

local function emit_burst(x, y, count, kind)
    local spawned = 0
    for _, particle in ipairs(particles) do
        if not particle.active then
            local smoke = spawned % 4 == 3
            local angle = math.random() * math.pi * 2
            local speed = smoke and math.random(12, 32) or math.random(48, 132)
            particle.active = true
            particle.x, particle.y = x, y
            particle.vx = math.cos(angle) * speed
            particle.vy = math.sin(angle) * speed + (smoke and -14 or 0)
            particle.max_life = smoke and 0.75 + math.random() * 0.4 or 0.28 + math.random() * 0.42
            particle.life = particle.max_life
            particle.kind = smoke and 2 or kind
            spawned = spawned + 1
            if spawned >= count then return end
        end
    end
end

local function emit_shockwave(x, y, kind, speed)
    for _, ring in ipairs(shockwaves) do
        if not ring.active then
            ring.active = true
            ring.x, ring.y = x, y
            ring.radius = 3
            ring.speed = speed or 44
            ring.life = 0.5
            ring.kind = kind
            return
        end
    end
end

local function spawn_pickup(x, y)
    for _, pickup in ipairs(pickups) do
        if not pickup.active then
            pickup.active = true
            pickup.x, pickup.y = x, y
            pickup.phase = 0
            return
        end
    end
end

local function spawn_bullet(x, y, vx)
    for _, bullet in ipairs(bullets) do
        if not bullet.active then
            bullet.active = true
            bullet.x, bullet.y = x, y
            bullet.vx, bullet.vy = vx or 0, -220
            return
        end
    end
end

local function spawn_enemy_bullet(enemy)
    for _, bullet in ipairs(enemy_bullets) do
        if not bullet.active then
            local dx = player.x - enemy.x
            bullet.active = true
            bullet.x, bullet.y = enemy.x, enemy.y + enemy.radius
            bullet.vx = clamp(dx * 0.18, -42, 42)
            bullet.vy = enemy.kind == 4 and 132 or 106
            return
        end
    end
end

local function reset_game()
    score, health, kills, wave = 0, 100, 0, 1
    combo, combo_timer, shot_timer, overdrive = 1, 0, 0, 0
    intro_timer, wave_banner, damage_flash, shake_timer = 2.2, 1.8, 0, 0
    game_state, state_timer, spawn_serial = "playing", 0, 0
    player.x, player.y = width * 0.5, height * 0.78
    player.target_x, player.target_y = player.x, player.y
    player.bank, player.invulnerable = 0, 0
    clear_pool(enemies)
    clear_pool(bullets)
    clear_pool(enemy_bullets)
    clear_pool(particles)
    clear_pool(shockwaves)
    clear_pool(pickups)
    sync_enemies(true)
    request_sfx("start")
end

local function damage_player(amount, x, y)
    if game_state ~= "playing" or player.invulnerable > 0 then return end
    health = math.max(0, health - amount)
    player.invulnerable = 0.65
    damage_flash = 0.24
    shake_timer = math.max(shake_timer, 0.28)
    emit_burst(x, y, 12, 3)
    emit_shockwave(x, y, 3, 58)
    request_sfx("damage")
    if health == 0 then
        game_state = "gameover"
        state_timer = 3.2
        emit_burst(player.x, player.y, 28, 3)
        emit_shockwave(player.x, player.y, 3, 76)
        shake_timer = 0.7
        request_sfx("game_over")
    end
end

local function destroy_enemy(enemy)
    kills = kills + 1
    combo = combo_timer > 0 and math.min(5, combo + 1) or 1
    combo_timer = 2.3
    local base = enemy.kind == 1 and 40 or enemy.kind == 2 and 70 or enemy.kind == 3 and 120 or 280
    score = score + base * combo
    local burst_kind = enemy.kind == 4 and 4 or enemy.kind == 3 and 1 or 4
    emit_burst(enemy.x, enemy.y, enemy.kind == 4 and 22 or 10, burst_kind)
    emit_shockwave(enemy.x, enemy.y, burst_kind, enemy.kind == 4 and 68 or 42)
    shake_timer = math.max(shake_timer, enemy.kind == 4 and 0.42 or 0.12)
    request_sfx(enemy.kind == 4 and "elite_destroy" or "destroy")
    if kills % 7 == 0 then spawn_pickup(enemy.x, enemy.y) end
    local next_wave = math.floor(kills / 12) + 1
    if next_wave > wave then
        wave = next_wave
        wave_banner = 1.8
        sync_enemies(false)
        request_sfx("wave")
    end
    configure_enemy(enemy, false)
end

reset_game()

local function update_input(t)
    local point
    if info.touch_available then point = screen:touch().points[1] end
    if point then
        player.target_x = point.x
        player.target_y = clamp(point.y, HUD_HEIGHT + 24, height - 24)
        if game_state == "gameover" then reset_game() end
    else
        player.target_x = width * 0.5 + math.sin(t * 1.18) * width * 0.31 + math.sin(t * 2.7) * width * 0.05
        player.target_y = height * 0.78 + math.sin(t * 0.72) * height * 0.075
    end
end

local function update_stars(dt)
    for _, star in ipairs(stars) do
        star.y = star.y + star.speed * dt
        if star.y >= height then
            star.y = HUD_HEIGHT
            star.x = math.random(0, width - 1)
        end
    end
end

local function update_player(dt)
    local previous_x = player.x
    player.x = player.x + (player.target_x - player.x) * math.min(1, dt * 10)
    player.y = player.y + (player.target_y - player.y) * math.min(1, dt * 10)
    player.x = clamp(player.x, 18, width - 18)
    player.y = clamp(player.y, HUD_HEIGHT + 24, height - 25)
    local bank_target = clamp((player.x - previous_x) * 1.7, -5, 5)
    player.bank = player.bank + (bank_target - player.bank) * math.min(1, dt * 15)
    player.invulnerable = math.max(0, player.invulnerable - dt)

    if game_state ~= "playing" then return end
    shot_timer = shot_timer - dt
    if shot_timer <= 0 then
        if overdrive > 0 then
            spawn_bullet(player.x - 7, player.y - 10, -8)
            spawn_bullet(player.x + 7, player.y - 10, 8)
            shot_timer = 0.11
        else
            spawn_bullet(player.x, player.y - 16, 0)
            shot_timer = 0.19
        end
    end
end

local function update_projectiles(dt)
    for _, bullet in ipairs(bullets) do
        if bullet.active then
            bullet.x = bullet.x + bullet.vx * dt
            bullet.y = bullet.y + bullet.vy * dt
            if bullet.y < HUD_HEIGHT - 12 then bullet.active = false end
        end
    end
    for _, bullet in ipairs(enemy_bullets) do
        if bullet.active then
            bullet.x = bullet.x + bullet.vx * dt
            bullet.y = bullet.y + bullet.vy * dt
            if bullet.y > height + 8 or bullet.x < -8 or bullet.x > width + 8 then
                bullet.active = false
            elseif game_state == "playing" then
                local dx, dy = bullet.x - player.x, bullet.y - player.y
                if dx * dx + dy * dy < 100 then
                    bullet.active = false
                    damage_player(9, bullet.x, bullet.y)
                end
            end
        end
    end
end

local function update_enemies(dt, t)
    for _, enemy in ipairs(enemies) do
        if enemy.active then
            enemy.y = enemy.y + enemy.vy * dt
            local sway = enemy.kind == 1 and 16 or enemy.kind == 2 and 42 or enemy.kind == 3 and 10 or 54
            enemy.x = enemy.x + (enemy.vx + math.sin(t * (enemy.kind == 4 and 2.7 or 1.9) + enemy.phase) * sway) * dt
            if enemy.x < enemy.radius then enemy.x, enemy.vx = enemy.radius, math.abs(enemy.vx) end
            if enemy.x > width - enemy.radius then enemy.x, enemy.vx = width - enemy.radius, -math.abs(enemy.vx) end

            enemy.fire_timer = enemy.fire_timer - dt
            if enemy.fire_timer <= 0 and enemy.y > HUD_HEIGHT + 12 and enemy.y < player.y - 40 and game_state == "playing" then
                spawn_enemy_bullet(enemy)
                enemy.fire_timer = enemy.kind == 4 and 0.65 or 1.4 + math.random() * 1.2
            end

            local destroyed = false
            for _, bullet in ipairs(bullets) do
                if bullet.active then
                    local dx, dy = enemy.x - bullet.x, enemy.y - bullet.y
                    if dx * dx + dy * dy < (enemy.radius + 3) ^ 2 then
                        bullet.active = false
                        enemy.hp = enemy.hp - 1
                        emit_burst(bullet.x, bullet.y, enemy.hp <= 0 and 4 or 2, enemy.kind == 4 and 4 or 1)
                        if enemy.hp <= 0 then
                            destroy_enemy(enemy)
                            destroyed = true
                        end
                        break
                    end
                end
            end

            if not destroyed then
                local dx, dy = enemy.x - player.x, enemy.y - player.y
                if game_state == "playing" and dx * dx + dy * dy < (enemy.radius + 11) ^ 2 then
                    damage_player(enemy.kind == 3 and 20 or 14, enemy.x, enemy.y)
                    configure_enemy(enemy, false)
                elseif enemy.y > height + enemy.radius then
                    if game_state == "playing" then damage_player(4, player.x, height - 6) end
                    configure_enemy(enemy, false)
                end
            end
        end
    end
end

local function update_effects(dt)
    for _, particle in ipairs(particles) do
        if particle.active then
            particle.life = particle.life - dt
            if particle.life <= 0 then
                particle.active = false
            else
                particle.x = particle.x + particle.vx * dt
                particle.y = particle.y + particle.vy * dt
                local drag = particle.kind == 2 and 0.965 or 0.93
                particle.vx, particle.vy = particle.vx * drag, particle.vy * drag
            end
        end
    end
    for _, ring in ipairs(shockwaves) do
        if ring.active then
            ring.life = ring.life - dt
            ring.radius = ring.radius + ring.speed * dt
            if ring.life <= 0 then ring.active = false end
        end
    end
    for _, pickup in ipairs(pickups) do
        if pickup.active then
            pickup.y = pickup.y + 38 * dt
            pickup.phase = pickup.phase + dt * 5
            local dx, dy = pickup.x - player.x, pickup.y - player.y
            if game_state == "playing" and dx * dx + dy * dy < 18 ^ 2 then
                pickup.active = false
                overdrive = 5.5
                health = math.min(100, health + 10)
                score = score + 150
                emit_burst(pickup.x, pickup.y, 14, 3)
                emit_shockwave(pickup.x, pickup.y, 3, 64)
                request_sfx("pickup")
            elseif pickup.y > height + 12 then
                pickup.active = false
            end
        end
    end
end

local function update_world(dt, t)
    update_input(t)
    update_stars(dt)
    update_player(dt)
    update_projectiles(dt)
    update_enemies(dt, t)
    update_effects(dt)

    combo_timer = math.max(0, combo_timer - dt)
    if combo_timer == 0 then combo = 1 end
    overdrive = math.max(0, overdrive - dt)
    intro_timer = math.max(0, intro_timer - dt)
    wave_banner = math.max(0, wave_banner - dt)
    damage_flash = math.max(0, damage_flash - dt)
    shake_timer = math.max(0, shake_timer - dt)
    if game_state == "gameover" then
        state_timer = state_timer - dt
        if state_timer <= 0 then reset_game() end
    end
end

local function draw_background(t)
    -- Opaque bands cover the framebuffer without an extra full-screen clear.
    local band_height = math.ceil(height / #SKY_BANDS)
    for index, color in ipairs(SKY_BANDS) do
        local y = (index - 1) * band_height
        screen:fill_rect(0, y, width, math.min(band_height, height - y), color)
    end

    local nebula_y = math.floor(height * 0.43 + math.sin(t * 0.18) * 5)
    screen:fill_round_rect(-24, nebula_y, math.floor(width * 0.58), 13, 6, COLORS.nebula)
    screen:fill_round_rect(math.floor(width * 0.48), nebula_y + 22, math.floor(width * 0.44), 7, 3, COLORS.violet_soft)

    local planet_x, planet_y = width - 48, HUD_HEIGHT + 55
    screen:fill_circle(planet_x, planet_y, 42, COLORS.planet_outer)
    screen:fill_circle(planet_x - 5, planet_y + 3, 35, COLORS.planet_inner)
    screen:arc(planet_x, planet_y, 47, 198, 342, COLORS.planet_light)
    screen:arc(planet_x, planet_y, 51, 202, 338, COLORS.grid)

    for _, star in ipairs(stars) do
        local x, y = pixel(star.x), pixel(star.y)
        if star.layer == 1 then
            screen:fill_rect(x, y, 1, 1, COLORS.star_far)
        elseif star.layer == 2 then
            screen:fill_rect(x, y, 2, 2, COLORS.star_mid)
        else
            screen:line(x, y - 2, x, y + 2, COLORS.star_near)
        end
    end
end

local function draw_scout(enemy, x, y, r)
    screen:fill_circle(x, y, r + 3, COLORS.pink_soft)
    screen:fill_triangle(x, y + r, x - r - 4, y - 4, x - 2, y - 1, COLORS.pink)
    screen:fill_triangle(x, y + r, x + r + 4, y - 4, x + 2, y - 1, COLORS.pink)
    screen:fill_triangle(x, y + r - 2, x - 4, y - r, x + 4, y - r, COLORS.violet)
    screen:fill_circle(x, y, 3, COLORS.white)
end

local function draw_striker(enemy, x, y, r)
    screen:fill_circle(x, y, r + 4, COLORS.violet_soft)
    screen:fill_triangle(x, y - r, x - r, y, x, y + r + 3, COLORS.violet)
    screen:fill_triangle(x, y - r, x + r, y, x, y + r + 3, COLORS.pink)
    screen:fill_triangle(x - 3, y, x - r - 6, y + 4, x - r + 1, y + 7, COLORS.cyan_dark)
    screen:fill_triangle(x + 3, y, x + r + 6, y + 4, x + r - 1, y + 7, COLORS.cyan_dark)
    screen:fill_circle(x, y, 4, COLORS.amber_hot)
end

local function draw_tank(enemy, x, y, r)
    screen:fill_round_rect(x - r, y - r + 2, r * 2, r * 2 - 1, 5, COLORS.red)
    screen:fill_triangle(x - r + 2, y, x - r - 7, y + r, x - 3, y + r - 2, COLORS.pink)
    screen:fill_triangle(x + r - 2, y, x + r + 7, y + r, x + 3, y + r - 2, COLORS.pink)
    screen:fill_round_rect(x - 7, y - 7, 14, 15, 4, COLORS.navy)
    screen:fill_circle(x, y, 4, COLORS.amber)
    if enemy.hp < enemy.max_hp then
        screen:fill_rect(x - r, y - r - 5, r * 2, 2, COLORS.navy)
        screen:fill_rect(x - r, y - r - 5, math.max(1, math.floor(r * 2 * enemy.hp / enemy.max_hp)), 2, COLORS.green)
    end
end

local function draw_elite(enemy, x, y, r, t)
    screen:fill_circle(x, y, r + 5, COLORS.pink_soft)
    screen:fill_triangle(x, y - r - 5, x - 6, y - 5, x + 6, y - 5, COLORS.violet)
    screen:fill_triangle(x, y + r + 5, x - 6, y + 5, x + 6, y + 5, COLORS.pink)
    screen:fill_triangle(x - r - 5, y, x - 5, y - 6, x - 5, y + 6, COLORS.violet)
    screen:fill_triangle(x + r + 5, y, x + 5, y - 6, x + 5, y + 6, COLORS.pink)
    screen:fill_circle(x, y, r - 3, COLORS.navy)
    screen:stroke_circle(x, y, r, COLORS.white)
    screen:arc(x, y, r + 4, (t * 100) % 360, (t * 100) % 360 + 105, COLORS.cyan)
    screen:fill_circle(x, y, 6, COLORS.amber_hot)
    screen:fill_rect(x - r, y - r - 7, r * 2, 3, COLORS.navy)
    screen:fill_rect(x - r, y - r - 7, math.max(1, math.floor(r * 2 * enemy.hp / enemy.max_hp)), 3, COLORS.pink)
end

local function draw_enemies(t)
    for _, enemy in ipairs(enemies) do
        if enemy.active then
            local x, y, r = pixel(enemy.x), pixel(enemy.y), enemy.radius
            if enemy.kind == 1 then draw_scout(enemy, x, y, r)
            elseif enemy.kind == 2 then draw_striker(enemy, x, y, r)
            elseif enemy.kind == 3 then draw_tank(enemy, x, y, r)
            else draw_elite(enemy, x, y, r, t) end
        end
    end
end

local function draw_projectiles()
    for _, bullet in ipairs(bullets) do
        if bullet.active then
            local x, y = pixel(bullet.x), pixel(bullet.y)
            screen:line(x, y + 11, x, y + 3, COLORS.cyan_soft)
            screen:fill_round_rect(x - 1, y - 4, 3, 9, 1, overdrive > 0 and COLORS.white or COLORS.cyan)
        end
    end
    for _, bullet in ipairs(enemy_bullets) do
        if bullet.active then
            local x, y = pixel(bullet.x), pixel(bullet.y)
            screen:line(x, y - 7, x, y + 2, COLORS.red_soft)
            screen:fill_circle(x, y + 2, 3, COLORS.orange)
            screen:fill_rect(x, y + 3, 1, 4, COLORS.red)
        end
    end
end

local function draw_pickups(t)
    for _, pickup in ipairs(pickups) do
        if pickup.active then
            local x, y = pixel(pickup.x), pixel(pickup.y)
            local pulse = 9 + math.floor((math.sin(pickup.phase + t * 2) + 1) * 2)
            screen:stroke_circle(x, y, pulse, COLORS.cyan)
            screen:fill_triangle(x, y - 7, x - 7, y, x, y + 7, COLORS.violet)
            screen:fill_triangle(x, y - 7, x + 7, y, x, y + 7, COLORS.cyan)
            screen:fill_rect(x - 1, y - 4, 3, 9, COLORS.white)
            screen:fill_rect(x - 4, y - 1, 9, 3, COLORS.white)
        end
    end
end

local function draw_particles()
    for _, particle in ipairs(particles) do
        if particle.active then
            local x, y = pixel(particle.x), pixel(particle.y)
            if particle.kind == 2 then
                local ratio = particle.life / particle.max_life
                local radius = ratio > 0.55 and 3 or 2
                screen:fill_circle(x, y, radius, ratio > 0.4 and COLORS.smoke or COLORS.smoke_dim)
            else
                local tail_x = pixel(particle.x - particle.vx * 0.035)
                local tail_y = pixel(particle.y - particle.vy * 0.035)
                local color = particle.kind == 3 and COLORS.cyan or particle.kind == 4 and COLORS.pink or COLORS.amber
                screen:line(tail_x, tail_y, x, y, color)
                if particle.life > particle.max_life * 0.55 then screen:fill_rect(x, y, 2, 2, COLORS.white) end
            end
        end
    end
    for _, ring in ipairs(shockwaves) do
        if ring.active then
            local color = ring.kind == 3 and COLORS.cyan or ring.kind == 4 and COLORS.pink or COLORS.amber
            screen:stroke_circle(pixel(ring.x), pixel(ring.y), math.max(1, pixel(ring.radius)), color)
        end
    end
end

local function draw_player(t)
    if player.invulnerable > 0 and math.floor(player.invulnerable * 20) % 2 == 0 then return end
    local x, y = pixel(player.x), pixel(player.y)
    local bank = pixel(player.bank)
    local flame = 10 + math.floor((math.sin(t * 19) + 1) * 3)
    screen:fill_circle(x, y + 8, 14, COLORS.cyan_soft)
    screen:fill_triangle(x - 2 + bank, y - 17, x - 17, y + 12, x - 4, y + 7, COLORS.cyan_dark)
    screen:fill_triangle(x + 2 + bank, y - 17, x + 17, y + 12, x + 4, y + 7, COLORS.cyan)
    screen:fill_triangle(x + bank, y - 20, x - 7, y + 11, x + 7, y + 11, COLORS.white)
    screen:fill_triangle(x + bank, y - 12, x - 4, y + 6, x + 4, y + 6, COLORS.navy)
    screen:fill_round_rect(x - 3 + bank, y - 7, 7, 9, 3, overdrive > 0 and COLORS.amber_hot or COLORS.cyan)
    screen:line(x - 14, y + 9, x - 5, y + 5, COLORS.white)
    screen:line(x + 14, y + 9, x + 5, y + 5, COLORS.white)
    screen:fill_triangle(x - 6, y + 10, x - 1, y + flame + 11, x, y + 10, COLORS.orange)
    screen:fill_triangle(x, y + 10, x + 1, y + flame + 11, x + 6, y + 10, COLORS.amber_hot)
end

local function update_hud(now)
    if now - (hud.updated_at or 0) < 180 then return end
    hud.updated_at = now
    hud.score = string.format("SCORE %06d", math.min(score, 999999))
    hud.wave = string.format("WAVE %02d", math.min(wave, 99))
    hud.combo = combo > 1 and string.format("X%d COMBO", combo) or ""
    hud.status = overdrive > 0 and string.format("OVERDRIVE %.1f", overdrive) or ""
    if DEBUG_OVERLAY then hud.debug = string.format("FPS %d D %d TX %.1f", perf.fps, perf.draw_ms, perf.present_us / 1000) end
end

local function centered_x(text, glyph_width)
    return math.floor((width - #text * glyph_width) / 2)
end

local function draw_hud()
    screen:fill_rect(0, 0, width, HUD_HEIGHT, COLORS.panel)
    screen:fill_rect(0, HUD_HEIGHT - 1, width, 1, COLORS.panel_line)
    screen:text(9, 6, hud.score, BODY_TEXT)
    screen:text(width - #hud.wave * BODY_WIDTH - 9, 6, hud.wave, BODY_TEXT)
    local bar_width = width - 18
    screen:fill_round_rect(9, 24, bar_width, 5, 2, COLORS.red)
    screen:fill_round_rect(9, 24, math.max(1, math.floor(bar_width * health / 100)), 5, 2, health > 30 and COLORS.green or COLORS.amber)
    if hud.combo ~= "" then screen:text(centered_x(hud.combo, BODY_WIDTH), HUD_HEIGHT + 7, hud.combo, BODY_AMBER) end
    if hud.status ~= "" then screen:text(centered_x(hud.status, BODY_WIDTH), height - 20, hud.status, BODY_CYAN) end
    if DEBUG_OVERLAY then screen:text(7, height - 14, hud.debug, BODY_MUTED) end
end

local function draw_overlays()
    if damage_flash > 0 then
        local thickness = damage_flash > 0.12 and 8 or 4
        screen:fill_rect(0, 0, width, thickness, COLORS.red_soft)
        screen:fill_rect(0, height - thickness, width, thickness, COLORS.red_soft)
        screen:fill_rect(0, thickness, thickness, height - thickness * 2, COLORS.red_soft)
        screen:fill_rect(width - thickness, thickness, thickness, height - thickness * 2, COLORS.red_soft)
    elseif overdrive > 0 then
        screen:stroke_round_rect(3, 3, width - 6, height - 6, 10, COLORS.cyan_soft)
    end

    if intro_timer > 0 then
        screen:fill_round_rect(38, math.floor(height * 0.42), width - 76, 55, 12, COLORS.panel)
        screen:text(centered_x("NEON STRIKE", TITLE_WIDTH), math.floor(height * 0.42) + 8, "NEON STRIKE", TITLE_CYAN)
        screen:text(centered_x("TOUCH TO STEER", BODY_WIDTH), math.floor(height * 0.42) + 34, "TOUCH TO STEER", BODY_MUTED)
    elseif wave_banner > 0 then
        local label = "WAVE " .. tostring(wave)
        screen:text(centered_x(label, TITLE_WIDTH), 58, label, TITLE_TEXT)
    end

    if game_state == "gameover" then
        screen:fill_rect(0, HUD_HEIGHT, width, height - HUD_HEIGHT, COLORS.black_overlay)
        screen:text(centered_x("SYSTEM DOWN", TITLE_WIDTH), math.floor(height * 0.44), "SYSTEM DOWN", TITLE_RED)
        screen:text(centered_x("TOUCH TO RESTART", BODY_WIDTH), math.floor(height * 0.44) + 29, "TOUCH TO RESTART", BODY_TEXT)
    end
end

local function update_perf(now)
    perf.frames = perf.frames + 1
    local elapsed = now - perf.window_ms
    if elapsed >= 1000 then
        perf.fps = math.floor(perf.frames * 1000 / elapsed + 0.5)
        perf.frames = 0
        perf.window_ms = now
    end
end

local function render(t, now)
    local draw_start = system.millis()
    screen:begin()
    draw_background(t)

    local shake_x, shake_y = 0, 0
    if shake_timer > 0 then
        local strength = shake_timer > 0.25 and 3 or 2
        shake_x, shake_y = math.random(-strength, strength), math.random(-strength, strength)
    end
    screen:save()
    screen:translate(shake_x, shake_y)
    draw_pickups(t)
    draw_projectiles()
    draw_enemies(t)
    draw_player(t)
    draw_particles()
    screen:restore()

    update_hud(now)
    draw_hud()
    draw_overlays()
    perf.draw_ms = system.millis() - draw_start
    screen:present()
    -- Avoid allocating a stats table every frame in normal gameplay.
    if DEBUG_OVERLAY then perf.present_us = screen:stats().present_us end
    update_perf(system.millis())
end

local function run()
    local started, previous = system.millis(), system.millis()
    local next_frame = started
    while RUN_MS == 0 or system.millis() - started < RUN_MS do
        drain_sfx()
        local now = system.millis()
        local dt = math.min(0.075, math.max(0.001, (now - previous) / 1000))
        previous = now
        local t = (now - started) / 1000
        update_world(dt, t)
        render(t, now)
        next_frame = next_frame + FRAME_MS
        local wait_ms = next_frame - system.millis()
        if wait_ms > 0 then delay.delay_ms(wait_ms) else next_frame = system.millis() end
    end
end

local run_ok, run_err = xpcall(run, debug.traceback)
cleanup()
if not run_ok then print("[neon_strike] ERROR: " .. tostring(run_err)) end
