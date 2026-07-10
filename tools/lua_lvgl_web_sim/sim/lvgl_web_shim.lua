local display = require("display")
local touch = require("touch")

local lvgl = {}
local state = {
    initialized = false,
    width = 800,
    height = 480,
    screen = nil,
    indev = {},
    pressed_target = nil,
}

local methods = {}
local render

local function color(value, fallback)
    return value or fallback or "white"
end

local function opt(opts, key, fallback)
    if type(opts) == "table" and opts[key] ~= nil then
        return opts[key]
    end
    return fallback
end

local function font_size(style, fallback)
    if type(style.font) == "table" and style.font.size then
        return style.font.size
    end
    return style.font_size or fallback
end

local function object(kind, parent, opts)
    opts = opts or {}
    local obj = {
        kind = kind,
        parent = parent,
        children = {},
        style = {},
        opts = opts,
        x = opt(opts, "x", 0),
        y = opt(opts, "y", 0),
        w = opt(opts, "w", kind == "screen" and state.width or 120),
        h = opt(opts, "h", kind == "screen" and state.height or 36),
        text = opt(opts, "text", ""),
        min = opt(opts, "min", 0),
        max = opt(opts, "max", 100),
        value = opt(opts, "value", 0),
        checked = opt(opts, "checked", false),
        align = opt(opts, "align", "top_left"),
        handlers = {},
    }
    setmetatable(obj, { __index = methods })
    if parent and parent.children then
        parent.children[#parent.children + 1] = obj
    end
    obj:set_style(opts)
    return obj
end

local function resolve(obj, parent)
    local pw = parent and parent.w or state.width
    local ph = parent and parent.h or state.height
    local px = parent and parent.abs_x or 0
    local py = parent and parent.abs_y or 0
    local x = obj.x or 0
    local y = obj.y or 0
    local align = obj.align or "top_left"

    if align == "center" then
        x = math.floor((pw - obj.w) / 2) + x
        y = math.floor((ph - obj.h) / 2) + y
    elseif align == "top_mid" then
        x = math.floor((pw - obj.w) / 2) + x
    elseif align == "bottom_mid" then
        x = math.floor((pw - obj.w) / 2) + x
        y = ph - obj.h + y
    end
    obj.abs_x = px + x
    obj.abs_y = py + y
end

local function draw_text_box(obj, text, font_size)
    display.draw_text_aligned(obj.abs_x, obj.abs_y, obj.w, obj.h, text or obj.text or "", {
        color = color(obj.style.text_color, "#f5f7fa"),
        font_size = font_size or obj.style.font_size or 18,
        align = "center",
        valign = "middle",
    })
end

local function contains(obj, x, y)
    return obj.abs_x ~= nil and obj.abs_y ~= nil and
        x >= obj.abs_x and y >= obj.abs_y and
        x < obj.abs_x + obj.w and y < obj.abs_y + obj.h
end

local function hit_test(obj, x, y)
    if not obj or not contains(obj, x, y) then
        return nil
    end
    for i = #obj.children, 1, -1 do
        local hit = hit_test(obj.children[i], x, y)
        if hit then
            return hit
        end
    end
    return obj
end

local function emit(obj, event_name)
    local count = 0
    while obj do
        for _, handler in ipairs(obj.handlers or {}) do
            if handler.name == event_name or handler.name == "all" then
                handler.callback()
                count = count + 1
            end
        end
        obj = obj.parent
    end
    return count
end

local function update_pointer_value(obj, x)
    if obj and obj.kind == "slider" then
        local span = math.max(1, obj.max - obj.min)
        local rel = math.max(0, math.min(1, (x - obj.abs_x) / math.max(1, obj.w)))
        local next_value = math.floor(obj.min + rel * span + 0.5)
        if next_value ~= obj.value then
            obj.value = next_value
            emit(obj, "value_changed")
            if state.screen then
                render(state.screen, nil)
            end
            return 1
        end
    end
    return 0
end

function render(obj, parent)
    resolve(obj, parent)
    if obj.kind == "screen" then
        display.begin_frame({ clear = true, color = color(obj.style.bg_color, "black") })
    elseif obj.kind == "container" then
        display.fill_round_rect(obj.abs_x, obj.abs_y, obj.w, obj.h, obj.style.radius or 0,
            color(obj.style.bg_color, { r = 255, g = 255, b = 255, a = obj.style.bg_opa or 32 }))
        if (obj.style.border_width or 0) > 0 then
            display.draw_round_rect(obj.abs_x, obj.abs_y, obj.w, obj.h, obj.style.radius or 0,
                color(obj.style.border_color, "#4f8cff"))
        end
    elseif obj.kind == "label" then
        local tw, th = display.measure_text(obj.text, { font_size = obj.style.font_size or 22 })
        obj.w = obj.w or tw
        obj.h = obj.h or th
        resolve(obj, parent)
        draw_text_box(obj, obj.text, font_size(obj.style, 22))
    elseif obj.kind == "button" then
        display.fill_round_rect(obj.abs_x, obj.abs_y, obj.w, obj.h, obj.style.radius or 4,
            color(obj.style.bg_color, "#2f80ed"))
        draw_text_box(obj, obj.text, 18)
    elseif obj.kind == "checkbox" then
        local box = 18
        display.draw_rect(obj.abs_x, obj.abs_y + 4, box, box, color(obj.style.text_color, "#f5f7fa"))
        if obj.checked then
            display.fill_rect(obj.abs_x + 4, obj.abs_y + 8, box - 8, box - 8, color(obj.style.text_color, "#f5f7fa"))
        end
        display.draw_text(obj.abs_x + box + 8, obj.abs_y, obj.text, {
            color = color(obj.style.text_color, "#f5f7fa"),
            font_size = 18,
        })
    elseif obj.kind == "bar" or obj.kind == "slider" then
        local span = math.max(1, obj.max - obj.min)
        local frac = math.max(0, math.min(1, (obj.value - obj.min) / span))
        display.fill_round_rect(obj.abs_x, obj.abs_y, obj.w, obj.h, math.floor(obj.h / 2), color(obj.style.bg_color, "#263241"))
        display.fill_round_rect(obj.abs_x, obj.abs_y, math.floor(obj.w * frac), obj.h, math.floor(obj.h / 2), "#4f8cff")
        if obj.kind == "slider" then
            local kx = obj.abs_x + math.floor(obj.w * frac)
            display.fill_circle(kx, obj.abs_y + math.floor(obj.h / 2), obj.h, "#f5f7fa")
        end
    elseif obj.kind ~= "screen" then
        display.draw_round_rect(obj.abs_x, obj.abs_y, obj.w, obj.h, obj.style.radius or 3,
            color(obj.style.border_color, "#334155"))
        if obj.text and obj.text ~= "" then
            draw_text_box(obj, obj.text, font_size(obj.style, 14))
        end
    end

    local flow_y = obj.abs_y + (obj.style.pad or 0)
    for _, child in ipairs(obj.children) do
        if obj.flex and obj.flex.flow == "column" then
            child.x = math.floor((obj.w - child.w) / 2)
            child.y = flow_y - obj.abs_y
            flow_y = flow_y + child.h + (obj.style.pad_row or 8)
        end
        render(child, obj)
    end
    if obj.kind == "screen" then
        display.present()
        display.end_frame()
    end
end

function methods:set_style(opts)
    for k, v in pairs(opts or {}) do
        self.style[k] = v
    end
    return true
end

function methods:set_size(w, h)
    self.w = w
    self.h = h
    return true
end

function methods:set_pos(x, y)
    self.x = x
    self.y = y
    return true
end

function methods:set_flex(opts)
    self.flex = opts or {}
    return true
end

function methods:set_scroll(opts)
    self.scroll = opts or {}
    return true
end

function methods:set_text(text)
    self.text = tostring(text or "")
    return true
end

function methods:set_value(value)
    self.value = value
    return true
end

function methods:set_step(step)
    self.step = step
    return true
end

function methods:get_step()
    return self.step or 1
end

function methods:increment()
    self.value = (self.value or 0) + (self.step or 1)
    return true
end

function methods:decrement()
    self.value = (self.value or 0) - (self.step or 1)
    return true
end

function methods:step_next()
    return true
end

function methods:step_prev()
    return true
end

function methods:get_value()
    return self.value
end

function methods:set_range(min, max)
    self.min = min
    self.max = max
    return true
end

function methods:set_map(map)
    self.map = map
    return true
end

function methods:set_one_checked(value)
    self.one_checked = value
    return true
end

function methods:set_selected(value)
    self.selected = value
    return true
end

function methods:get_selected()
    return self.selected or 1
end

function methods:get_button_text(index)
    local visible = {}
    for _, item in ipairs(self.map or self.opts.map or {}) do
        if item ~= "\n" then
            visible[#visible + 1] = item
        end
    end
    return visible[index or self:get_selected()] or ""
end

function methods:set_color(value)
    self.style.bg_color = value
    self.style.color = value
    return true
end

function methods:set_brightness(value)
    self.brightness = value
    return true
end

function methods:get_brightness()
    return self.brightness or self.opts.brightness or 0
end

function methods:toggle()
    self.on_state = not self.on_state
    return true
end

function methods:fill_bg(value)
    self.style.bg_color = value
    return true
end

function methods:set_px(x, y, value)
    self.pixels = self.pixels or {}
    self.pixels[x .. "," .. y] = value
    return true
end

function methods:get_px(x, y)
    local c = self.pixels and self.pixels[x .. "," .. y] or "#000000"
    if type(c) == "string" and c:sub(1, 1) == "#" then
        return {
            r = tonumber(c:sub(2, 3), 16) or 0,
            g = tonumber(c:sub(4, 5), 16) or 0,
            b = tonumber(c:sub(6, 7), 16) or 0,
        }
    end
    return c
end

function methods:add_series(color_value, axis)
    local series = { color = color_value, axis = axis, values = {} }
    self.series = self.series or {}
    self.series[#self.series + 1] = series
    return series
end

function methods:set_type(value) self.chart_type = value return true end
function methods:set_point_count(value) self.point_count = value return true end
function methods:set_series_values(series, values) series.values = values return true end
function methods:set_next_value(series, value) series.values[#series.values + 1] = value return true end
function methods:refresh() return true end
function methods:set_src(state_name, src) self.src = src self.state_name = state_name return true end
function methods:set_state(state_name) self.state_name = state_name return true end
function methods:set_today(year, month, day) self.today = { year, month, day } return true end
function methods:set_shown(year, month) self.shown = { year, month } return true end
function methods:set_highlighted(days) self.highlighted = days return true end
function methods:get_pressed_date() return nil end

function methods:add_tab(name)
    self.tabs = self.tabs or {}
    local tab = object("container", self, { text = name, w = self.w, h = math.max(32, self.h - 28), bg_opa = 0 })
    self.tabs[#self.tabs + 1] = tab
    return tab
end

function methods:get_tab_count()
    return #(self.tabs or {})
end

function methods:set_active(index)
    self.active = index
    return true
end

function methods:get_active()
    return self.active or 1
end

function methods:set_tab_text(index, text)
    if self.tabs and self.tabs[index] then
        self.tabs[index].text = text
    end
    return true
end

function methods:add_tile(col, row, dir)
    self.tiles = self.tiles or {}
    local tile = object("container", self, { text = "tile", w = self.w, h = self.h, bg_opa = 0 })
    tile.tile_col = col
    tile.tile_row = row
    tile.dir = dir
    self.tiles[#self.tiles + 1] = tile
    return tile
end

function methods:set_tile(tile)
    self.active_tile = tile
    return true
end

function methods:get_active_tile()
    return self.active_tile or (self.tiles and self.tiles[1])
end

function methods:set_tile_by_index(col, row)
    for _, tile in ipairs(self.tiles or {}) do
        if tile.tile_col == col and tile.tile_row == row then
            self.active_tile = tile
            return true
        end
    end
    return true
end

function methods:add_span(text, style)
    self.spans = self.spans or {}
    local span = { text = text or "", style = style or {}, parent = self }
    function span:set_text(value) self.text = value return true end
    function span:get_text() return self.text end
    function span:set_style(opts) for k, v in pairs(opts or {}) do self.style[k] = v end return true end
    function span:delete()
        local keep = {}
        for _, item in ipairs(self.parent.spans or {}) do
            if item ~= self then keep[#keep + 1] = item end
        end
        self.parent.spans = keep
        return true
    end
    self.spans[#self.spans + 1] = span
    return span
end

function methods:get_span_count()
    return #(self.spans or {})
end

function methods:set_mode_header(value) self.mode_header = value return true end
function methods:set_root_back_button(value) self.root_back_button = value return true end
function methods:page(name) return object("container", self, { text = name, w = self.w, h = self.h, bg_opa = 0 }) end
function methods:section(parent) return object("container", parent or self, { w = self.w, h = 28, bg_opa = 0 }) end
function methods:cont(parent) return object("container", parent or self, { w = self.w, h = 28, bg_opa = 0 }) end
function methods:separator(parent) return object("container", parent or self, { w = self.w, h = 2, bg_color = "#334155" }) end
function methods:set_sidebar_page(page) self.sidebar_page = page return true end
function methods:set_page(page) self.page_obj = page return true end
function methods:clear_history() return true end
function methods:add_title(text) return object("label", self, { text = text, w = self.w, h = 24 }) end
function methods:add_button(src, size) return object("button", self, { text = "", w = size or 30, h = size or 30 }) end
function methods:get_header() self.header = self.header or object("container", self, { w = self.w, h = 28 }) return self.header end
function methods:get_content() self.content = self.content or object("container", self, { w = self.w, h = math.max(24, self.h - 28), y = 28 }) return self.content end
function methods:add_text(text) return object("label", self, { text = text, w = self.w, h = 22 }) end
function methods:add_footer_button(text) return object("button", self, { text = text, w = 72, h = 28 }) end
function methods:add_close_button() return object("button", self, { text = "x", w = 28, h = 28 }) end
function methods:close() self.closed = true return true end
function methods:close_async() self.closed = true return true end

function methods:load()
    state.screen = self
    render(self, nil)
    return true
end

local function fake_userdata()
    return io.tmpfile()
end

function methods:on(name, callback)
    local handle = fake_userdata()
    self.handlers[#self.handlers + 1] = {
        name = name,
        callback = callback,
        handle = handle,
    }
    return handle
end

function methods:off(match)
    local removed = 0
    local keep = {}
    for _, handler in ipairs(self.handlers or {}) do
        local drop = match == nil or match == handler.name or match == handler.handle
        if drop then
            removed = removed + 1
        else
            keep[#keep + 1] = handler
        end
    end
    self.handlers = keep
    return removed
end

function lvgl.init(panel_handle, io_handle, width, height, panel_if, opts)
    display._claim_owner("lvgl")
    display.init(panel_handle, io_handle, width, height, panel_if)
    state.initialized = true
    state.width = width or state.width
    state.height = height or state.height
    return true
end

function lvgl.deinit()
    if state.initialized then
        display.deinit()
        display._release_owner("lvgl")
    end
    state.initialized = false
    return true
end

function lvgl.create_screen()
    return object("screen", nil, { w = state.width, h = state.height })
end

function lvgl.label(parent, opts) return object("label", parent, opts) end
function lvgl.container(parent, opts) return object("container", parent, opts) end
function lvgl.button(parent, opts) return object("button", parent, opts) end
function lvgl.checkbox(parent, opts) return object("checkbox", parent, opts) end
function lvgl.bar(parent, opts) return object("bar", parent, opts) end
function lvgl.slider(parent, opts) return object("slider", parent, opts) end
function lvgl.buttonmatrix(parent, opts) return object("buttonmatrix", parent, opts) end
function lvgl.led(parent, opts) return object("led", parent, opts) end
function lvgl.spinbox(parent, opts) return object("spinbox", parent, opts) end
function lvgl.calendar(parent, opts) return object("calendar", parent, opts) end
function lvgl.canvas(parent, opts) return object("canvas", parent, opts) end
function lvgl.chart(parent, opts) return object("chart", parent, opts) end
function lvgl.imagebutton(parent, opts) return object("imagebutton", parent, opts) end
function lvgl.tabview(parent, opts) return object("tabview", parent, opts) end
function lvgl.tileview(parent, opts) return object("tileview", parent, opts) end
function lvgl.spangroup(parent, opts)
    local obj = object("spangroup", parent, opts)
    obj.spans = {}
    for _, text in ipairs(opts and opts.spans or {}) do
        obj:add_span(text)
    end
    return obj
end
function lvgl.menu(parent, opts) return object("menu", parent, opts) end
function lvgl.window(parent, opts) return object("window", parent, opts) end
function lvgl.msgbox(parent, opts)
    local obj = object("msgbox", parent, opts)
    if opts and opts.title then obj:add_title(opts.title) end
    if opts and opts.text then obj:add_text(opts.text) end
    return obj
end

local function process_one_event()
    local event = touch.poll()
    if not event or not state.screen then
        return 0
    end
    local target = hit_test(state.screen, event.x, event.y)
    local count = 0
    if event.pressed then
        state.pressed_target = target
        if target then
            count = count + emit(target, "pressed")
            count = count + update_pointer_value(target, event.x)
        end
    else
        if target then
            count = count + update_pointer_value(target, event.x)
        end
        if target and state.pressed_target == target then
            count = count + emit(target, "clicked")
        end
        state.pressed_target = nil
    end
    return count
end

function lvgl.process_events(timeout_ms)
    local count = process_one_event()
    while count > 0 do
        local next_count = process_one_event()
        if next_count == 0 then
            break
        end
        count = count + next_count
    end
    return count
end

function lvgl.run()
    return 0
end

function lvgl.indev_register(name)
    if state.indev[name] then
        error("lvgl indev already registered")
    end
    state.indev[name] = true
    return true
end

function lvgl.indev_unregister(name)
    if not state.indev[name] then
        return false
    end
    state.indev[name] = nil
    return true
end

function lvgl.demos()
    return { "widgets" }
end

function lvgl.demo(name)
    if not state.initialized then
        error("lvgl runtime is not initialized")
    end
    if name ~= "widgets" then
        error("demo unavailable")
    end
    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#101820" })
    lvgl.label(scr, { text = "LVGL widgets demo", align = "center", text_color = "white" })
    scr:load()
    return true
end

function lvgl.font_load(path, opts)
    if not path then
        error("font path required")
    end
    return { path = path, size = opt(opts, "size", 24) }
end

return lvgl
