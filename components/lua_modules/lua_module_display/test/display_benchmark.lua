local display = require("display")
local system = require("system")

local options = type(args) == "table" and args or {}

local function integer_option(name, default, minimum, maximum)
    local value = math.tointeger(options[name] or default)
    assert(value and value >= minimum and value <= maximum,
           string.format("%s must be an integer in [%d, %d]", name, minimum, maximum))
    return value
end

local repeats = integer_option("repeats", 3, 1, 7)
local warmup_iterations = integer_option("warmup_iterations", 3, 1, 20)
local frame_iterations = integer_option("frame_iterations", 24, 4, 200)
local raster_iterations = integer_option("raster_iterations", 800, 100, 10000)
local requested_buffers = integer_option("framebuffer_count", 0, 0, 2)
local label = tostring(options.label or "unspecified")
local backend_name = type(display.open) == "function" and "refactored" or "legacy"

local active = nil
local info = nil
local text_options = { color = "#f0f4f8", font_size = 16 }
local full_present_options = { full = true }

-- Keep workloads identical across the legacy and current public APIs.
local function now_us()
    return math.floor(system.millis() * 1000)
end

local function heap_snapshot()
    local caps = system.heap.caps
    local internal = system.heap.get_info(caps.INTERNAL)
    local psram = system.heap.get_info(caps.SPIRAM)
    return { sram = internal.free_size, psram = psram.free_size }
end

local function open_display(framebuffer_count)
    if backend_name == "refactored" then
        active = display.open({ framebuffer_count = framebuffer_count })
        info = active:info()
        return
    end

    local board_manager = require("board_manager")
    local panel, panel_io, width, height, panel_if = board_manager.get_display_lcd_params("display_lcd")
    assert(panel, "built-in display is unavailable")
    if framebuffer_count == 1 then
        display.init(panel, panel_io, width, height, panel_if)
    else
        display.init(panel, panel_io, width, height, panel_if, nil, { framebuffer_count = framebuffer_count })
    end
    active = display
    info = {
        width = display.width,
        height = display.height,
        pixel_format = display.pixel_format,
        bytes_per_pixel = display.bytes_per_pixel,
        framebuffer_count = framebuffer_count,
        framebuffer_bytes = display.width * display.height * display.bytes_per_pixel * framebuffer_count,
    }
end

local function close_display()
    if active == nil then return end
    if backend_name == "refactored" then
        active:close()
    else
        pcall(display.end_frame)
        display.deinit()
    end
    active = nil
    info = nil
end

local function begin_frame(clear_color)
    if backend_name == "refactored" then
        if clear_color then active:begin({ clear = clear_color }) else active:begin() end
    else
        if clear_color then
            display.begin_frame({ clear = true, color = clear_color })
        else
            display.begin_frame({ clear = false, preserve = true })
        end
    end
end

local function present_frame(full)
    if backend_name == "refactored" then
        if full then active:present(full_present_options) else active:present() end
    else
        if full then display.present_full() else display.present() end
        display.end_frame()
    end
end

local function fill_rect(x, y, width, height, color)
    if backend_name == "refactored" then
        active:fill_rect(x, y, width, height, color)
    else
        display.fill_rect(x, y, width, height, color)
    end
end

local function stroke_rect(x, y, width, height, color)
    if backend_name == "refactored" then
        active:stroke_rect(x, y, width, height, color)
    else
        display.draw_rect(x, y, width, height, color)
    end
end

local function draw_line(x0, y0, x1, y1, color)
    if backend_name == "refactored" then
        active:line(x0, y0, x1, y1, color)
    else
        display.draw_line(x0, y0, x1, y1, color)
    end
end

local function fill_circle(cx, cy, radius, color)
    if backend_name == "refactored" then
        active:fill_circle(cx, cy, radius, color)
    else
        display.fill_circle(cx, cy, radius, color)
    end
end

local function stroke_circle(cx, cy, radius, color)
    if backend_name == "refactored" then active:stroke_circle(cx, cy, radius, color) else display.draw_circle(cx, cy, radius, color) end
end

local function draw_arc(cx, cy, radius, start_angle, end_angle, color)
    if backend_name == "refactored" then active:arc(cx, cy, radius, start_angle, end_angle, color) else display.draw_arc(cx, cy, radius, start_angle, end_angle, color) end
end

local function fill_round_rect(x, y, width, height, radius, color)
    if backend_name == "refactored" then active:fill_round_rect(x, y, width, height, radius, color) else display.fill_round_rect(x, y, width, height, radius, color) end
end

local function fill_triangle(x0, y0, x1, y1, x2, y2, color)
    if backend_name == "refactored" then active:fill_triangle(x0, y0, x1, y1, x2, y2, color) else display.fill_triangle(x0, y0, x1, y1, x2, y2, color) end
end

local function draw_text(x, y, text)
    if backend_name == "refactored" then
        active:text(x, y, text, text_options)
    else
        display.draw_text(x, y, text, text_options)
    end
end

local function draw_pixels(x, y, pixels, pixel_options)
    if backend_name == "refactored" then
        active:blit(x, y, pixels, pixel_options)
    else
        display.draw_pixels(x, y, pixels, pixel_options)
    end
end

local function summarize(values)
    table.sort(values)
    local median = values[(#values + 1) // 2]
    return median, values[1], values[#values]
end

local function emit_result(kind, name, framebuffer_count, iterations, values, pixels_per_iteration)
    local median, minimum, maximum = summarize(values)
    median = math.max(median, 1)
    local us_per_operation = math.floor(median / iterations + 0.5)
    local operations_per_second = math.floor(iterations / median * 1000000 + 0.5)
    local pixels_per_second = pixels_per_iteration and math.floor(iterations * pixels_per_iteration / median * 1000000 + 0.5) or 0
    print(string.format("DISPLAY_BENCH|result|label=%s|backend=%s|kind=%s|case=%s|buffers=%d|iterations=%d|median_us=%d|min_us=%d|max_us=%d|us_per_op=%d|ops_per_s=%d|pixels_per_s=%d",
        label, backend_name, kind, name, framebuffer_count, iterations, median, minimum, maximum,
        us_per_operation, operations_per_second, pixels_per_second))
end

local function run_frame_case(name, framebuffer_count, iterations, pixels_per_iteration, render)
    local function workload(count)
        local started = now_us()
        for iteration = 1, count do render(iteration) end
        return now_us() - started
    end

    workload(warmup_iterations)
    local values = {}
    for repeat_index = 1, repeats do values[repeat_index] = workload(iterations) end
    emit_result("frame", name, framebuffer_count, iterations, values, pixels_per_iteration)
end

local function run_raster_case(name, framebuffer_count, iterations, render)
    -- Exclude display transfer time to isolate raster and binding cost.
    local function workload(count)
        begin_frame("#101820")
        local started = now_us()
        for iteration = 1, count do render(iteration) end
        local elapsed = now_us() - started
        present_frame(true)
        return elapsed
    end

    workload(math.min(iterations, 40))
    local values = {}
    for repeat_index = 1, repeats do values[repeat_index] = workload(iterations) end
    emit_result("raster", name, framebuffer_count, iterations, values, nil)
end

local function run_buffer_suite(framebuffer_count)
    collectgarbage("collect")
    local heap_before = heap_snapshot()
    local open_started = now_us()
    open_display(framebuffer_count)
    local open_elapsed = now_us() - open_started
    -- Include lazy framebuffer allocation in ready time and memory usage.
    begin_frame("#101820")
    present_frame(true)
    local ready_elapsed = now_us() - open_started
    collectgarbage("collect")
    local heap_open = heap_snapshot()
    local width, height = info.width, info.height
    local tile = math.min(64, width, height)
    local scene_iterations = math.max(4, frame_iterations // 2)

    print(string.format("DISPLAY_BENCH|memory|label=%s|backend=%s|buffers=%d|width=%d|height=%d|format=%s|fb_bytes=%d|open_us=%d|ready_us=%d|sram_used=%d|psram_used=%d",
        label, backend_name, framebuffer_count, width, height, info.pixel_format, info.framebuffer_bytes,
        open_elapsed, ready_elapsed, heap_before.sram - heap_open.sram, heap_before.psram - heap_open.psram))

    run_frame_case("partial_64", framebuffer_count, frame_iterations, tile * tile, function(iteration)
        begin_frame(nil)
        local x_range = math.max(1, width - tile + 1)
        local y_range = math.max(1, height - tile + 1)
        local x = (iteration * 37) % x_range
        local y = (iteration * 53) % y_range
        fill_rect(x, y, tile, tile, iteration % 2 == 0 and "#2060a0" or "#70b030")
        present_frame(false)
    end)

    local scatter = math.min(32, width // 3, height // 3)
    run_frame_case("scattered_2x32", framebuffer_count, frame_iterations, scatter * scatter * 2, function(iteration)
        begin_frame(nil)
        local color = iteration % 2 == 0 and "#8040c0" or "#30a080"
        fill_rect(0, 0, scatter, scatter, color)
        fill_rect(width - scatter, height - scatter, scatter, scatter, color)
        present_frame(false)
    end)

    run_frame_case("full_fill", framebuffer_count, frame_iterations, width * height, function(iteration)
        begin_frame(iteration % 2 == 0 and "#182840" or "#301828")
        present_frame(true)
    end)

    run_frame_case("complex_scene", framebuffer_count, scene_iterations, width * height, function(iteration)
        begin_frame("#101820")
        for index = 1, 20 do
            local x = (index * 29 + iteration * 7) % math.max(1, width)
            local y = (index * 43 + iteration * 11) % math.max(1, height)
            fill_rect(x - 8, y - 6, 24, 18, index % 2 == 0 and "#2878b8" or "#d06030")
            draw_line(width // 2, height // 2, x, y, "#80d0f0")
            fill_circle(x, y, 6, "#50c070")
        end
        draw_text(8, 8, "Display benchmark 0123456789")
        present_frame(true)
    end)

    run_raster_case("fill_rect", framebuffer_count, raster_iterations, function(iteration)
        local x = (iteration * 17) % math.max(1, width)
        local y = (iteration * 31) % math.max(1, height)
        fill_rect(x, y, 12, 12, "#4080c0")
    end)

    run_raster_case("line", framebuffer_count, raster_iterations, function(iteration)
        local y0 = (iteration * 13) % math.max(1, height)
        local y1 = (iteration * 47) % math.max(1, height)
        draw_line(0, y0, width - 1, y1, "#f0a040")
    end)

    run_raster_case("fill_circle", framebuffer_count, raster_iterations, function(iteration)
        local x = (iteration * 19) % math.max(1, width)
        local y = (iteration * 23) % math.max(1, height)
        fill_circle(x, y, 8, "#40c080")
    end)

    run_raster_case("stroke_rect", framebuffer_count, raster_iterations, function(iteration)
        local x = (iteration * 17) % math.max(1, width)
        local y = (iteration * 31) % math.max(1, height)
        stroke_rect(x, y, 24, 18, "#d08040")
    end)

    run_raster_case("stroke_circle", framebuffer_count, raster_iterations, function(iteration)
        local x = (iteration * 19) % math.max(1, width)
        local y = (iteration * 23) % math.max(1, height)
        stroke_circle(x, y, 12, "#70c0e0")
    end)

    local shape_iterations = math.max(50, raster_iterations // 4)
    run_raster_case("arc", framebuffer_count, shape_iterations, function(iteration)
        local x = (iteration * 19) % math.max(1, width)
        local y = (iteration * 23) % math.max(1, height)
        draw_arc(x, y, 16, 25, 300, "#e0a050")
    end)

    run_raster_case("fill_round_rect", framebuffer_count, shape_iterations, function(iteration)
        local x = (iteration * 17) % math.max(1, width)
        local y = (iteration * 29) % math.max(1, height)
        fill_round_rect(x, y, 30, 22, 6, "#7060d0")
    end)

    run_raster_case("fill_triangle", framebuffer_count, shape_iterations, function(iteration)
        local x = (iteration * 13) % math.max(1, width)
        local y = (iteration * 31) % math.max(1, height)
        fill_triangle(x, y, x + 24, y + 5, x + 9, y + 22, "#40b080")
    end)

    local text_iterations = math.max(50, raster_iterations // 4)
    run_raster_case("text", framebuffer_count, text_iterations, function(iteration)
        local y_range = math.max(1, height - 16)
        draw_text((iteration * 11) % math.max(1, width), (iteration * 17) % y_range, "Display 0123456789")
    end)

    local pixel_size = 16
    local pixel
    if info.pixel_format == "rgb565" then
        pixel = string.char(0x1f, 0x7c)
    else
        pixel = string.char(0x78, 0x84, 0xf0)
    end
    local pixel_block = string.rep(pixel, pixel_size * pixel_size)
    local pixel_options = { width = pixel_size, height = pixel_size, format = info.pixel_format }
    local blit_iterations = math.max(50, raster_iterations // 4)
    run_raster_case("blit_16", framebuffer_count, blit_iterations, function(iteration)
        local x = (iteration * 29) % math.max(1, width)
        local y = (iteration * 37) % math.max(1, height)
        draw_pixels(x, y, pixel_block, pixel_options)
    end)

    close_display()
    collectgarbage("collect")
    local heap_after = heap_snapshot()
    print(string.format("DISPLAY_BENCH|memory_after_close|label=%s|backend=%s|buffers=%d|sram_delta=%d|psram_delta=%d",
        label, backend_name, framebuffer_count, heap_before.sram - heap_after.sram, heap_before.psram - heap_after.psram))
end

local buffer_counts = requested_buffers == 0 and { 1, 2 } or { requested_buffers }
print(string.format("DISPLAY_BENCH|begin|schema=2|label=%s|backend=%s|repeats=%d|warmup=%d|frame_iterations=%d|raster_iterations=%d",
    label, backend_name, repeats, warmup_iterations, frame_iterations, raster_iterations))

local ok, err = xpcall(function()
    for _, framebuffer_count in ipairs(buffer_counts) do run_buffer_suite(framebuffer_count) end
end, debug.traceback)

if not ok then
    pcall(close_display)
    print(string.format("DISPLAY_BENCH|error|label=%s|backend=%s|message=%s", label, backend_name, tostring(err):gsub("[\r\n]+", " ")))
    error(err, 0)
end

print(string.format("DISPLAY_BENCH|end|schema=2|label=%s|backend=%s|status=PASS", label, backend_name))
