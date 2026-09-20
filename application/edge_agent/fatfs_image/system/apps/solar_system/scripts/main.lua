local display = require("display")
local delay = require("delay")
local system = require("system")

local script_dir = assert(debug.getinfo(1, "S").source:match("^@(.*/)"), "script directory unavailable")
local app_dir = assert(script_dir:match("^(.*)/scripts/$"), "app directory unavailable")
local data = dofile(script_dir .. "solar_data.lua")
local Scene = dofile(script_dir .. "solar_scene.lua")
local app_args = type(args) == "table" and args or {}

local function int_arg(name, default, min_value, max_value)
    local value = type(app_args[name]) == "number" and math.floor(app_args[name]) or default
    return math.max(min_value, math.min(max_value, value))
end

local frame_ms = int_arg("frame_ms", 33, 16, 200)
local run_ms = int_arg("run_ms", 0, 0, 3600000)
local framebuffer_count = int_arg("framebuffer_count", 1, 1, 2)
local ok, screen_or_err = pcall(display.open, { framebuffer_count = framebuffer_count })
if not ok then
    print("[solar_system] ERROR: open failed: " .. tostring(screen_or_err))
    return
end

local screen = screen_or_err
local opened = true
local fonts = {}

local function cleanup()
    if fonts.title then pcall(fonts.title.close, fonts.title) end
    if fonts.label then pcall(fonts.label.close, fonts.label) end
    fonts.title, fonts.label = nil, nil
    if opened then
        pcall(screen.close, screen)
        opened = false
    end
end

local function run()
    local info = screen:info()
    if info.width <= 0 or info.height <= 0 then error("invalid display size") end
    fonts.label = display.load_font(app_dir .. "/assets/planet_labels_12.dfn")
    fonts.title = display.load_font(app_dir .. "/assets/title_18.dfn")
    local scene = Scene.new(screen, info, data, fonts)
    local started = system.millis()
    local previous = started
    local next_frame = started
    while run_ms == 0 or system.millis() - started < run_ms do
        local now = system.millis()
        local dt = math.min(0.1, math.max(0.001, (now - previous) / 1000))
        previous = now
        scene:update_touch(dt)
        scene:render((now - started) / 1000)
        next_frame = next_frame + frame_ms
        local wait_ms = next_frame - system.millis()
        if wait_ms > 0 then delay.delay_ms(wait_ms) else next_frame = system.millis() end
    end
end

local run_ok, run_err = xpcall(run, debug.traceback)
cleanup()
if not run_ok then print("[solar_system] ERROR: " .. tostring(run_err)) end
