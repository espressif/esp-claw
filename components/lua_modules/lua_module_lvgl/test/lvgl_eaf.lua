local board_manager = require("board_manager")
local delay = require("delay")
local lvgl = require("lvgl")
local storage = require("storage")

local function check(cond, msg)
    if not cond then
        error(msg or "check failed", 2)
    end
end

local function check_eq(actual, expected, msg)
    if actual ~= expected then
        error((msg or "check_eq failed") .. ": expected " .. tostring(expected) .. ", got " .. tostring(actual), 2)
    end
end

local function data_path_for(rel_path)
    return storage.join_path(storage.get_root_dir(), rel_path)
end

local function lvgl_fs_path_for(rel_path)
    return "D:/" .. rel_path
end

local test_args = args or {}
local eaf_rel_path = test_args.eaf_rel_path or "eafs/test.eaf"
local eaf_src = test_args.eaf_src or lvgl_fs_path_for(eaf_rel_path)
local eaf_data_path = data_path_for(eaf_rel_path)

if not storage.exists(eaf_data_path) then
    error("missing test EAF: place an EAF at " .. eaf_data_path .. " or pass args.eaf_rel_path")
end

local panel_handle, io_handle, width, height, panel_if =
    board_manager.get_display_lcd_params("display_lcd")

lvgl.init(panel_handle, io_handle, width, height, panel_if, {
    buffer_lines = 10,
    tick_ms = 5,
    task_period_ms = 10,
})

local ok, err = pcall(function()
    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#101820" })

    lvgl.label(scr, {
        text = "LVGL EAF playback",
        align = "top_mid",
        y = 16,
        text_color = "#f5f7fa",
    })

    local side = math.floor(math.min(width, height) * 3 / 5)
    local anim = lvgl.eaf(scr, {
        src = eaf_src,
        align = "center",
        w = side,
        h = side,
        loop_count = -1,
        loop_enabled = true,
        frame_delay = 50,
    })

    check(anim:is_loaded(), "EAF should load from " .. eaf_src)
    check(anim:get_total_frames() > 0, "EAF should report total frames")
    check(anim:get_current_frame() >= 0, "EAF should report current frame")
    check_eq(anim:get_loop_count(), -1, "EAF loop count")
    check_eq(anim:get_loop_enabled(), true, "EAF loop enabled")
    check_eq(anim:get_frame_delay(), 50, "EAF frame delay")

    anim:pause()
    delay.delay_ms(100)
    anim:resume()
    delay.delay_ms(300)
    anim:restart()

    anim:set_loop_enabled(false)
    check_eq(anim:get_loop_enabled(), false, "EAF loop disabled")
    anim:set_loop_count(1)
    check_eq(anim:get_loop_count(), 1, "EAF loop count update")
    anim:set_frame_delay(80)
    check_eq(anim:get_frame_delay(), 80, "EAF frame delay update")
    anim:set_loop_enabled(true)
    anim:set_loop_count(-1)

    scr:load()
    delay.delay_ms(test_args.duration_ms or 3000)
end)

if not ok then
    error(err)
end
