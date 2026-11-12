-- Run standalone for visual inspection, or pass {group, probe} from a native
-- harness. The public Lua API has no style getter; the probe reads LVGL styles.
local context = ...
local external_group = type(context) == "table" and context.group ~= nil
local lvgl

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

if not external_group then
    lvgl = require("lvgl")
    lvgl.init({ buffer_lines = 10, tick_ms = 5, task_period_ms = 10 })
end

local ok, err = pcall(function()
    local scr = not external_group and lvgl.create_screen() or nil
    local group = external_group and context.group or lvgl.spangroup(scr, {
        w = 220,
        h = 60,
        mode = "break",
        text_color = "#112233",
        pad = 7,
    })
    local first = group:add_span("first ", { text_color = "#22aa44" })
    local second = group:add_span("second ", { text_color = "#ffaa00", text_decor = 1 })
    local third = group:add_span("inherited")
    check_eq(group:get_span_count(), 3, "mixed span count")

    local function verify(phase)
        check_eq(first:get_text(), "first ", "first sibling text preserved")
        check_eq(second:get_text(), "second ", "styled span text preserved")
        check_eq(third:get_text(), "inherited", "inherited sibling text preserved")
        if external_group then
            context.probe(group, phase)
        end
    end

    verify(1)
    check(second:set_style({ text_color = "#8844cc", text_decor = 2 }), "set span style")
    verify(2)
    check(second:set_style({}), "empty span style")
    verify(2)
    check(second:set_style({ text_color = 0x8844cc }), "partial numeric span style")
    verify(2)

    if not external_group then
        scr:load()
        print("Inspect: first stays green, second becomes purple/struck through, inherited stays #112233")
        print("Group padding stays 7; visual style checks are not automatic without the native probe")
        for _ = 1, 20 do
            lvgl.process_events(500)
        end
    end
end)

if not external_group then
    local deinit_ok, deinit_err = pcall(lvgl.deinit)
    if not deinit_ok then
        print("lvgl.deinit failed: " .. tostring(deinit_err))
    end
end
if not ok then
    error(err)
end
