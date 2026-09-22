local system = require("system")

local Scene = {}
Scene.__index = Scene

local label_candidates = { { 1, -1 }, { 1, 1 }, { -1, -1 }, { -1, 1 } }
local jupiter_bands = { -5, -2, 2, 5 }
local TOUCH_NONE, TOUCH_ROTATE, TOUCH_TRANSFORM = 0, 1, 2
local MIN_ZOOM, MAX_ZOOM = 0.65, 2.40
local MIN_PINCH_DISTANCE = 4
local PAN_LIMIT_RATIO = 0.75

local function clamp(value, minimum, maximum)
    return math.max(minimum, math.min(maximum, value))
end

local function boxes_overlap(a, b)
    return a.x < b.x + b.width + 2 and a.x + a.width + 2 > b.x and a.y < b.y + b.height + 2 and a.y + a.height + 2 > b.y
end

function Scene.new(screen, info, data, fonts)
    local self = setmetatable({}, Scene)
    self.screen = screen
    self.width, self.height = info.width, info.height
    self.touch_available = info.touch_available
    self.data, self.colors, self.planets = data, data.colors, data.planets
    self.fonts = fonts
    self.center_x, self.center_y = self.width // 2, self.height // 2 + 7
    self.orbit_scale = math.min(self.width, self.height) * 0.415
    self.camera = { yaw = -0.42, pitch = 0.90, zoom = 1, yaw_velocity = 0, pitch_velocity = 0, cy = 0, sy = 0, cp = 0, sp = 0 }
    self.touch = { mode = TOUCH_NONE, id = 0, x = 0, y = 0, pair_a = 0, pair_b = 0, mid_x = 0, mid_y = 0, distance = 0 }
    self.render_order = { 1, 2, 3, 4, 5, 6, 7, 8, 9 }
    self.label_boxes, self.stars, self.asteroids = {}, {}, {}

    for _, planet in ipairs(self.planets) do
        planet.x, planet.y, planet.depth, planet.scale = 0, 0, 0, 1
        local ok, text_width, text_height = pcall(screen.measure_text, screen, planet.name, { font = fonts.label })
        planet.label_width = ok and text_width or #planet.name * 6
        planet.label_height = ok and text_height or 12
    end

    math.randomseed(system.millis() & 0x7fffffff)
    for i = 1, 86 do
        self.stars[i] = { x = math.random(1, self.width - 2), y = math.random(28, self.height - 3), hot = i % 13 == 0, bright = i % 4 == 0 }
    end
    for i = 1, 50 do
        local angle = math.random() * data.two_pi
        local radius = 0.465 + math.random() * 0.055
        self.asteroids[i] = { x = math.cos(angle) * radius, y = (math.random() - 0.5) * 0.014, z = math.sin(angle) * radius }
    end
    return self
end

function Scene:project(x, y, z)
    local camera = self.camera
    local rx = x * camera.cy - z * camera.sy
    local rz = x * camera.sy + z * camera.cy
    local py = y * camera.cp - rz * camera.sp
    local depth = y * camera.sp + rz * camera.cp
    local perspective = 1 / math.max(0.72, 1 + depth * 0.18)
    local scale = self.orbit_scale * camera.zoom
    return math.floor(self.center_x + rx * scale * perspective + 0.5), math.floor(self.center_y + py * scale * perspective + 0.5), depth, perspective
end

function Scene:orbit_position(planet, eccentric_anomaly)
    local local_x = planet.orbit * (math.cos(eccentric_anomaly) - planet.eccentricity)
    local local_z = planet.orbit * planet.minor * math.sin(eccentric_anomaly)
    return local_x * planet.cn - local_z * planet.ci * planet.sn, local_z * planet.si, local_x * planet.sn + local_z * planet.ci * planet.cn
end

function Scene:solve_eccentric_anomaly(mean_anomaly, eccentricity)
    local value = mean_anomaly + eccentricity * math.sin(mean_anomaly)
    for _ = 1, 3 do
        value = value - (value - eccentricity * math.sin(value) - mean_anomaly) / (1 - eccentricity * math.cos(value))
    end
    return value
end

function Scene:clamp_center()
    local margin = math.max(self.width, self.height) * PAN_LIMIT_RATIO
    self.center_x = math.floor(clamp(self.center_x, -margin, self.width + margin) + 0.5)
    self.center_y = math.floor(clamp(self.center_y, -margin, self.height + margin) + 0.5)
end

function Scene:update_two_finger(first, second)
    local touch, camera = self.touch, self.camera
    local pair_a, pair_b = first.id, second.id
    if pair_a > pair_b then pair_a, pair_b = pair_b, pair_a end
    local dx, dy = second.x - first.x, second.y - first.y
    local distance = math.max(MIN_PINCH_DISTANCE, math.sqrt(dx * dx + dy * dy))
    local mid_x, mid_y = (first.x + second.x) * 0.5, (first.y + second.y) * 0.5

    if touch.mode == TOUCH_TRANSFORM and touch.pair_a == pair_a and touch.pair_b == pair_b then
        local old_zoom = camera.zoom
        camera.zoom = clamp(old_zoom * distance / touch.distance, MIN_ZOOM, MAX_ZOOM)
        local applied_zoom = camera.zoom / old_zoom
        self.center_x = mid_x + (self.center_x - touch.mid_x) * applied_zoom
        self.center_y = mid_y + (self.center_y - touch.mid_y) * applied_zoom
        self:clamp_center()
    end

    camera.yaw_velocity, camera.pitch_velocity = 0, 0
    touch.mode, touch.pair_a, touch.pair_b = TOUCH_TRANSFORM, pair_a, pair_b
    touch.mid_x, touch.mid_y, touch.distance = mid_x, mid_y, distance
end

function Scene:update_single_finger(point)
    local touch, camera = self.touch, self.camera
    if touch.mode == TOUCH_ROTATE and touch.id == point.id then
        local dx, dy = point.x - touch.x, point.y - touch.y
        camera.yaw = camera.yaw + dx * 0.012
        camera.pitch = clamp(camera.pitch + dy * 0.008, 0.18, 1.46)
        camera.yaw_velocity = clamp(dx * 0.20, -2.4, 2.4)
        camera.pitch_velocity = clamp(dy * 0.12, -1.5, 1.5)
    else
        camera.yaw_velocity, camera.pitch_velocity = 0, 0
    end
    touch.mode, touch.id, touch.x, touch.y = TOUCH_ROTATE, point.id, point.x, point.y
end

function Scene:update_touch(dt)
    if not self.touch_available then return end
    local points = self.screen:touch().points
    local touch, camera = self.touch, self.camera
    if points[2] then
        self:update_two_finger(points[1], points[2])
    elseif points[1] then
        self:update_single_finger(points[1])
    else
        touch.mode = TOUCH_NONE
        camera.yaw = camera.yaw + camera.yaw_velocity * dt
        camera.pitch = clamp(camera.pitch + camera.pitch_velocity * dt, 0.18, 1.46)
        local damping = math.max(0, 1 - dt * 4.5)
        camera.yaw_velocity = camera.yaw_velocity * damping
        camera.pitch_velocity = camera.pitch_velocity * damping
    end
    if camera.yaw > math.pi then camera.yaw = camera.yaw - self.data.two_pi end
    if camera.yaw < -math.pi then camera.yaw = camera.yaw + self.data.two_pi end
end

function Scene:draw_background()
    local screen, colors = self.screen, self.colors
    for _, star in ipairs(self.stars) do
        local color = star.hot and colors.star_hot or (star.bright and colors.star or colors.star_dim)
        if star.hot then
            screen:line(star.x - 1, star.y, star.x + 1, star.y, color)
            screen:line(star.x, star.y - 1, star.x, star.y + 1, color)
        else
            screen:fill_rect(star.x, star.y, 1, 1, color)
        end
    end
end

function Scene:draw_orbit(planet)
    local screen, colors = self.screen, self.colors
    local first = planet.orbit_points[1]
    local x1, y1, depth1 = self:project(first[1], first[2], first[3])
    for i = 2, #planet.orbit_points do
        local point = planet.orbit_points[i]
        local x2, y2, depth2 = self:project(point[1], point[2], point[3])
        screen:line(x1, y1, x2, y2, (depth1 + depth2) > 0 and colors.orbit_far or colors.orbit_near)
        x1, y1, depth1 = x2, y2, depth2
    end
end

function Scene:draw_asteroid_belt()
    for _, asteroid in ipairs(self.asteroids) do
        local x, y = self:project(asteroid.x, asteroid.y, asteroid.z)
        self.screen:fill_rect(x, y, 1, 1, self.colors.asteroid)
    end
end

function Scene:draw_sun_glow()
    local zoom = self.camera.zoom
    self.screen:fill_circle(self.center_x, self.center_y, math.max(10, math.floor(31 * zoom + 0.5)), self.colors.sun_glow_outer)
    self.screen:fill_circle(self.center_x, self.center_y, math.max(8, math.floor(23 * zoom + 0.5)), self.colors.sun_glow_inner)
end

function Scene:draw_sun(t)
    local screen, colors = self.screen, self.colors
    local radius = math.max(8, math.floor(15 * self.camera.zoom + 0.5))
    screen:fill_circle(self.center_x, self.center_y, radius, colors.sun_edge)
    screen:fill_circle(self.center_x, self.center_y, radius - 1, colors.sun)
    local spin = math.floor((t * 3) % 9)
    screen:fill_circle(self.center_x - 6 + spin, self.center_y + 3, 1, colors.sun_spot)
    screen:fill_circle(self.center_x + 6 - spin // 2, self.center_y - 5, 1, colors.sun_spot)
    screen:arc(self.center_x, self.center_y, radius - 3, 205 + t * 8, 258 + t * 8, colors.sun_hot)
    screen:arc(self.center_x, self.center_y, radius - 6, 18 - t * 11, 82 - t * 11, colors.sun_hot)
end

function Scene:planet_radius(planet)
    return math.max(2, math.floor(planet.radius * planet.scale * self.data.body_scale * self.camera.zoom + 0.5))
end

function Scene:shade_planet(planet, radius)
    local lx, ly, lz = self.center_x - planet.x, self.center_y - planet.y, planet.depth * self.orbit_scale * self.camera.zoom
    local length = math.sqrt(lx * lx + ly * ly + lz * lz)
    if length < 0.001 then length = 1 end
    lx, ly, lz = lx / length, ly / length, lz / length
    for yy = -radius, radius do
        local span = math.floor(math.sqrt(math.max(0, radius * radius - yy * yy)))
        local run_start, run_color = -span, nil
        for xx = -span, span do
            local surface = math.sqrt(math.max(0, radius * radius - xx * xx - yy * yy))
            local light = (xx * lx + yy * ly + surface * lz) / radius
            local color = light < -0.18 and planet.colors[1] or (light < 0.28 and planet.colors[2] or (light < 0.70 and planet.colors[3] or planet.colors[4]))
            if run_color and color ~= run_color then
                self.screen:line(planet.x + run_start, planet.y + yy, planet.x + xx - 1, planet.y + yy, run_color)
                run_start = xx
            end
            run_color = color
        end
        self.screen:line(planet.x + run_start, planet.y + yy, planet.x + span, planet.y + yy, run_color)
    end
end

function Scene:draw_ring_half(planet, radius, front)
    local ring_x = radius * 2 + 3
    local ring_y = math.max(2, math.floor(ring_x * math.sin(self.camera.pitch) * 0.27))
    local rotation = 0.16 + self.camera.yaw * 0.10
    local cr, sr = math.cos(rotation), math.sin(rotation)
    local segments = 30
    local function ring_point(angle)
        local ex, ey = math.cos(angle) * ring_x, math.sin(angle) * ring_y
        return math.floor(planet.x + ex * cr - ey * sr + 0.5), math.floor(planet.y + ex * sr + ey * cr + 0.5)
    end
    local x1, y1 = ring_point(0)
    for i = 1, segments do
        local angle = i * self.data.two_pi / segments
        local x2, y2 = ring_point(angle)
        local is_front = math.sin(angle - self.data.two_pi / segments * 0.5) > 0
        if is_front == front then self.screen:line(x1, y1, x2, y2, front and self.colors.ring_front or self.colors.ring_back) end
        x1, y1 = x2, y2
    end
end

function Scene:draw_planet_texture(planet, radius, t)
    local screen, colors = self.screen, self.colors
    if planet.name == "EARTH" and radius >= 4 then
        local drift = math.floor((t * 2) % 4) - 2
        screen:fill_circle(planet.x - 1 + drift, planet.y - 1, 1, colors.earth_land)
        screen:fill_circle(planet.x + 2 + drift // 2, planet.y + 2, 1, colors.earth_dry)
        screen:line(planet.x - 3, planet.y - 3, planet.x + 1, planet.y - 3, colors.earth_cloud)
    elseif planet.name == "MARS" then
        screen:line(planet.x - 1, planet.y - radius + 1, planet.x + 1, planet.y - radius + 1, colors.mars_ice)
        screen:fill_rect(planet.x + 1, planet.y + 1, 1, 1, colors.mars_dark)
    elseif planet.name == "JUPITER" then
        for _, offset in ipairs(jupiter_bands) do
            local span = math.floor(math.sqrt(math.max(0, radius * radius - offset * offset))) - 1
            screen:line(planet.x - span, planet.y + offset, planet.x + span, planet.y + offset, offset == 2 and colors.jupiter_belt or colors.jupiter_band)
        end
        screen:fill_circle(planet.x + 4, planet.y + 3, 2, colors.jupiter_spot)
    elseif planet.name == "SATURN" then
        screen:line(planet.x - radius + 2, planet.y - 2, planet.x + radius - 2, planet.y - 2, colors.saturn_light)
        screen:line(planet.x - radius + 1, planet.y + 2, planet.x + radius - 1, planet.y + 2, colors.saturn_belt)
    elseif planet.name == "VENUS" then
        screen:line(planet.x - radius + 1, planet.y - 1, planet.x + radius - 1, planet.y - 1, colors.venus_cloud)
    elseif planet.name == "NEPTUNE" then
        screen:line(planet.x - radius + 2, planet.y + 1, planet.x + radius - 2, planet.y + 1, colors.neptune_cloud)
    elseif planet.name == "MERCURY" then
        screen:fill_rect(planet.x + 1, planet.y, 1, 1, colors.mercury_crater)
    end
end

function Scene:draw_planet(planet, t)
    local radius = self:planet_radius(planet)
    if planet.rings then self:draw_ring_half(planet, radius, false) end
    self:shade_planet(planet, radius)
    self:draw_planet_texture(planet, radius, t)
    if planet.rings then self:draw_ring_half(planet, radius, true) end
end

function Scene:update_planets(sim_days)
    for _, planet in ipairs(self.planets) do
        local mean_anomaly = (planet.phase + sim_days / planet.period * self.data.two_pi) % self.data.two_pi
        local eccentric_anomaly = self:solve_eccentric_anomaly(mean_anomaly, planet.eccentricity)
        local wx, wy, wz = self:orbit_position(planet, eccentric_anomaly)
        planet.x, planet.y, planet.depth, planet.scale = self:project(wx, wy, wz)
    end
    table.sort(self.render_order, function(a, b)
        local depth_a = a == 1 and 0 or self.planets[a - 1].depth
        local depth_b = b == 1 and 0 or self.planets[b - 1].depth
        return depth_a > depth_b
    end)
end

function Scene:draw_bodies(t)
    for _, body_index in ipairs(self.render_order) do
        if body_index == 1 then self:draw_sun(t) else self:draw_planet(self.planets[body_index - 1], t) end
    end
end

function Scene:draw_labels()
    local boxes = self.label_boxes
    for i = #boxes, 1, -1 do boxes[i] = nil end
    local sun_clearance = math.max(20, math.floor(24 * self.camera.zoom + 0.5))
    boxes[1] = { x = self.center_x - sun_clearance, y = self.center_y - sun_clearance, width = sun_clearance * 2, height = sun_clearance * 2 }
    for _, planet in ipairs(self.planets) do
        local radius = self:planet_radius(planet)
        local chosen
        for distance_step = 0, 2 do
            local distance = radius + 5 + distance_step * 10
            for _, candidate in ipairs(label_candidates) do
                local label_x = candidate[1] > 0 and planet.x + distance or planet.x - distance - planet.label_width
                local label_y = candidate[2] > 0 and planet.y + 4 + distance_step * 4 or planet.y - planet.label_height - 4 - distance_step * 4
                local box = { x = label_x, y = label_y, width = planet.label_width, height = planet.label_height }
                local valid = label_x >= 4 and label_x + box.width < self.width - 4 and label_y >= 32 and label_y + box.height < self.height - 4
                if valid then
                    for _, occupied in ipairs(boxes) do
                        if boxes_overlap(box, occupied) then valid = false break end
                    end
                end
                if valid then chosen = box break end
            end
            if chosen then break end
        end
        if chosen then
            boxes[#boxes + 1] = chosen
            local anchor_x = chosen.x > planet.x and chosen.x - 2 or chosen.x + chosen.width + 2
            local anchor_y = chosen.y + chosen.height // 2
            local dx, dy = anchor_x - planet.x, anchor_y - planet.y
            local length = math.max(1, math.sqrt(dx * dx + dy * dy))
            local line_x = math.floor(planet.x + dx / length * (radius + 1) + 0.5)
            local line_y = math.floor(planet.y + dy / length * (radius + 1) + 0.5)
            self.screen:line(line_x, line_y, anchor_x, anchor_y, self.colors.orbit_near)
            self.screen:text(chosen.x, chosen.y, planet.name, { font = self.fonts.label, color = self.colors.label })
        end
    end
end

function Scene:render(t)
    local camera = self.camera
    camera.cy, camera.sy = math.cos(camera.yaw), math.sin(camera.yaw)
    camera.cp, camera.sp = math.cos(camera.pitch), math.sin(camera.pitch)
    self:update_planets(t * self.data.sim_days_per_second)

    self.screen:begin({ clear = self.colors.space })
    self:draw_background()
    self:draw_sun_glow()
    for _, planet in ipairs(self.planets) do self:draw_orbit(planet) end
    self:draw_asteroid_belt()
    self:draw_bodies(t)
    self:draw_labels()
    self.screen:text(10, 7, "SOLAR SYSTEM", { font = self.fonts.title, color = self.colors.title })
    self.screen:present()
end

return Scene
