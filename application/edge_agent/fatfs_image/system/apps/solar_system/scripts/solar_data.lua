local display = require("display")

local data = {
    two_pi = math.pi * 2,
    orbit_segments = 36,
    sim_days_per_second = 12,
    body_scale = 1.12,
}

data.colors = {
    space = display.color(1, 3, 8), star_dim = display.color(74, 86, 103), star = display.color(157, 169, 184), star_hot = display.color(235, 239, 244),
    orbit_far = display.color(48, 57, 69, 105), orbit_near = display.color(91, 104, 120, 145), asteroid = display.color(102, 93, 79, 150),
    title = display.color(239, 242, 245), label = display.color(202, 211, 221),
    sun_glow_outer = display.color(255, 132, 25, 20), sun_glow_inner = display.color(255, 177, 38, 48),
    sun_edge = display.color(255, 139, 23), sun = display.color(255, 195, 54), sun_hot = display.color(255, 235, 147), sun_spot = display.color(181, 83, 24, 180),
    ring_back = display.color(116, 101, 75, 170), ring_front = display.color(211, 190, 140, 220),
    earth_land = display.color(83, 121, 72), earth_dry = display.color(117, 130, 73), earth_cloud = display.color(226, 235, 231, 180),
    mars_ice = display.color(224, 197, 166), mars_dark = display.color(83, 32, 24), mercury_crater = display.color(55, 51, 48),
    jupiter_band = display.color(218, 190, 153, 210), jupiter_belt = display.color(158, 91, 60), jupiter_spot = display.color(166, 72, 50),
    saturn_light = display.color(231, 211, 160, 210), saturn_belt = display.color(119, 98, 63, 190),
    venus_cloud = display.color(235, 202, 139, 190), neptune_cloud = display.color(78, 124, 210, 220),
}

local function palette(sr, sg, sb, br, bg, bb, lr, lg, lb, hr, hg, hb)
    return { display.color(sr, sg, sb), display.color(br, bg, bb), display.color(lr, lg, lb), display.color(hr, hg, hb) }
end

-- Distances are log-scaled; periods, eccentricities, and inclinations retain real relative values.
data.planets = {
    { name = "MERCURY", au = 0.387, period = 87.97, eccentricity = 0.2056, inclination = 7.00, node = 48.3, radius = 3, phase = 0.35, colors = palette(36, 33, 31, 91, 85, 79, 151, 142, 130, 208, 197, 180) },
    { name = "VENUS", au = 0.723, period = 224.70, eccentricity = 0.0068, inclination = 3.39, node = 76.7, radius = 4, phase = 2.10, colors = palette(62, 42, 26, 145, 102, 57, 210, 165, 91, 244, 213, 146) },
    { name = "EARTH", au = 1.000, period = 365.26, eccentricity = 0.0167, inclination = 0.00, node = 0.0, radius = 5, phase = 3.65, colors = palette(5, 22, 42, 19, 74, 127, 48, 139, 185, 148, 210, 230) },
    { name = "MARS", au = 1.524, period = 686.98, eccentricity = 0.0934, inclination = 1.85, node = 49.6, radius = 4, phase = 5.15, colors = palette(47, 22, 16, 118, 50, 31, 188, 85, 48, 233, 146, 93) },
    { name = "JUPITER", au = 5.203, period = 4332.59, eccentricity = 0.0489, inclination = 1.30, node = 100.5, radius = 10, phase = 0.90, colors = palette(55, 44, 37, 129, 106, 88, 197, 165, 128, 239, 218, 181) },
    { name = "SATURN", au = 9.537, period = 10759.22, eccentricity = 0.0565, inclination = 2.49, node = 113.7, radius = 8, phase = 2.55, colors = palette(62, 54, 38, 142, 125, 82, 211, 192, 134, 244, 229, 181), rings = true },
    { name = "URANUS", au = 19.191, period = 30688.5, eccentricity = 0.0472, inclination = 0.77, node = 74.0, radius = 6, phase = 4.05, colors = palette(18, 49, 57, 63, 132, 142, 127, 198, 203, 201, 237, 233) },
    { name = "NEPTUNE", au = 30.069, period = 60182.0, eccentricity = 0.0086, inclination = 1.77, node = 131.8, radius = 6, phase = 5.45, colors = palette(10, 23, 67, 27, 58, 145, 52, 103, 208, 132, 171, 237) },
}

local min_log = math.log(data.planets[1].au)
local log_range = math.log(data.planets[#data.planets].au) - min_log
for _, planet in ipairs(data.planets) do
    planet.orbit = 0.17 + (math.log(planet.au) - min_log) / log_range * 0.81
    planet.inclination = math.rad(planet.inclination)
    planet.node = math.rad(planet.node)
    planet.cn, planet.sn = math.cos(planet.node), math.sin(planet.node)
    planet.ci, planet.si = math.cos(planet.inclination), math.sin(planet.inclination)
    planet.minor = math.sqrt(1 - planet.eccentricity * planet.eccentricity)
    planet.orbit_points = {}
    for i = 0, data.orbit_segments do
        local angle = i * data.two_pi / data.orbit_segments
        local local_x = planet.orbit * (math.cos(angle) - planet.eccentricity)
        local local_z = planet.orbit * planet.minor * math.sin(angle)
        planet.orbit_points[i + 1] = {
            local_x * planet.cn - local_z * planet.ci * planet.sn,
            local_z * planet.si,
            local_x * planet.sn + local_z * planet.ci * planet.cn,
        }
    end
end

return data
