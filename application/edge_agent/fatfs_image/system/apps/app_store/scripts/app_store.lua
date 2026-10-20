local lvgl, storage, json = require("lvgl"), require("storage"), require("json")
local thread, image, delay = require("thread"), require("image"), require("delay")
local scripts = assert(debug.getinfo(1, "S").source:match("^@(.*)/[^/]+$"))
local root = storage.get_root_dir()
local cache = storage.join_path(root, "cache", "app_store")
local server = ((args or {}).server or "https://skills-lab.esp-claw.com"):gsub("/+$", "")
local job, queue = "app_store_worker", "app_store_progress"
local items, selected, task, confirm = {}, nil, nil, false
local running, closing, dirty, owned = true, false, true, false
local tab, notice, percent = 1, "正在准备应用目录", 0
local screen, body, status, bar, width, height, font
local home, list, empty, detail_page, detail_view, active_tab
local cards, tabs = {}, {}
local C = { bg = "#0b0b0d", card = "#19191c", text = "#f3f3f5", muted = "#ababb3", accent = "#97ff65", tint = "#26262b", line = "#303036", danger = "#ff7788" }

local function read(path)
    local ok, value = pcall(function() return json.decode(storage.read_file(path)) end)
    return ok and type(value) == "table" and value or nil
end

local function erase(path)
    if storage.exists(path) then storage.remove(path) end
end

local function worker_active()
    local ok, info = thread.get(job)
    local state = ok and info:match("\nstatus=(%w+)")
    return state == "running" or state == "queued"
end

local function installed(id)
    if storage.exists("/system/apps/" .. id .. "/launcher.json") then return "内置" end
    if storage.exists(root .. "/apps/" .. id .. "/launcher.json") then return "已安装" end
end

local function reload()
    items = read(cache .. "/catalog.json") or {}
    local seen = {}
    for _, item in ipairs(items) do seen[item.id] = true end
    for _, base in ipairs({ root .. "/apps", "/system/apps" }) do
        if storage.exists(base) then
            for _, entry in ipairs(storage.listdir(base)) do
                if entry.type == "dir" and not seen[entry.name] then
                    local m = read(base .. "/" .. entry.name .. "/launcher.json")
                    if m and m.id == entry.name then
                        items[#items + 1] = { id = m.id, title = m.display_name or m.id, description = (m.catalog or {}).description or "本地应用", metadata = m.catalog or {} }
                        seen[m.id] = true
                    end
                end
            end
        end
    end
    dirty = true
end

local function compact(text, pixels)
    text = tostring(text):gsub("%s+", " ")
    local used, last = 0, 0
    for pos, cp in utf8.codes(text) do
        used = used + (cp < 128 and 10 or 18)
        if used > pixels - 18 then return text:sub(1, last) .. "…" end
        last = (utf8.offset(text, 2, pos) or (#text + 1)) - 1
    end
    return text
end

local function label(parent, text, x, y, w, h, color)
    -- The bundled device font omits the multiplication sign used by some descriptions.
    return lvgl.label(parent, { text = tostring(text):gsub("×", "x"), x = x, y = y, w = w, h = h or 24, text_color = color or C.text, font = font })
end

local function button(parent, text, x, y, w, fn, accent)
    local b = lvgl.button(parent, { text = text, x = x, y = y, w = w, h = 44, radius = 8, pad = 0, border_width = 1, border_color = accent and (type(accent) == "string" and accent or C.accent) or C.line, bg_color = accent and (type(accent) == "string" and accent or C.accent) or C.card, text_color = accent and C.bg or C.text, font = font })
    b:on("clicked", fn)
    return b
end

local function icon(parent, item, x, y, box)
    local path = cache .. "/" .. item.id .. ".jpg"
    box = box or lvgl.object(parent, { x = x, y = y, w = 48, h = 48, radius = 8, pad = 0, border_width = 0, bg_color = C.tint })
    box:clear_flag("SCROLLABLE")
    box:clear_flag("CLICKABLE")
    if not storage.exists(path) then label(box, "APP", 4, 12, 44, 24, C.muted); return box, false end
    local frames = {}
    local ok, err = pcall(function()
        frames[1] = image.load_file(path)
        frames[2] = image.resize(frames[1], { width = 48, height = 48 })
        box:clean()
        local view = lvgl.image(box, { x = 0, y = 0 })
        view:set_raw_rgb565(48, 48, frames[2]:data())
    end)
    for _, frame in ipairs(frames) do frame:release() end
    if not ok then print("[AppStore] image: " .. tostring(err)); box:clean(); label(box, "APP", 4, 12, 44, 24, C.muted) end
    return box, ok
end

local function start(action, item)
    if task or worker_active() then notice = "后台任务进行中，请稍候"; return end
    erase(cache .. "/result.json")
    erase(cache .. "/cancel")
    while thread.sync.queue_recv(queue, 0) do end
    local ok, err = thread.start(scripts .. "/store_worker.lua", { action = action, id = item and item.id, size = item and item.totalSize, server = server, queue = queue }, { name = job, exclusive = job, replace = false, timeout_ms = 0 })
    if not ok then print("[AppStore] start: " .. tostring(err)); notice = "后台任务启动失败"; return end
    task, notice, percent, confirm, dirty = action, "正在连接服务器", 0, false, true
end

local function exit()
    if task or worker_active() then
        storage.write_file(cache .. "/cancel", "1")
        closing, notice = true, "正在取消，请稍候"
    else running = false end
end

local function render_catalog(w, h)
    if not home then
        home = lvgl.object(body, { x = 0, y = 0, w = w, h = h, bg_opa = 0, pad = 0, border_width = 0 })
        home:clear_flag("SCROLLABLE")
        local tw = math.floor((w - 12) / 4)
        for i, name in ipairs({ "全部", "游戏", "工具", "已装" }) do
            tabs[i] = button(home, name, (i - 1) * (tw + 4), 0, tw, function() tab, dirty = i, true end, tab == i)
        end
        active_tab = tab
        list = lvgl.object(home, { x = 0, y = 52, w = w, h = h - 52, bg_opa = 0, pad = 0, border_width = 0 })
        list:set_scroll({ dir = "ver", scrollbar = "auto" })
        empty = label(list, "暂无应用，点击右上角刷新", 0, 12, w, 60, C.muted)
        empty:add_flag("HIDDEN")
    end
    if active_tab ~= tab then
        tabs[active_tab]:set_style({ border_color = C.line, bg_color = C.card, text_color = C.text })
        tabs[tab]:set_style({ border_color = C.accent, bg_color = C.accent, text_color = C.bg })
        active_tab = tab
    end
    -- Keep card objects and the scroll container alive across catalog refreshes.
    local seen, count = {}, 0
    for _, item in ipairs(items) do
        seen[item.id] = true
        local categories, state = (item.metadata or {}).category or {}, installed(item.id)
        local category = "," .. table.concat(categories, ",") .. ","
        local visible = tab == 1 or (tab == 2 and category:find(",game,", 1, true)) or (tab == 3 and category:find(",utility,", 1, true)) or (tab == 4 and state)
        local card = cards[item.id]
        if visible then
            if not card then
                card = { item = item, y = count * 80 }
                card.obj = lvgl.object(list, { x = 0, y = card.y, w = w - 4, h = 72, radius = 10, pad = 0, border_width = 1, border_color = C.line, bg_color = C.card })
                card.obj:clear_flag("SCROLLABLE")
                card.obj:add_flag("CLICKABLE")
                card.obj:on("clicked", function() selected, confirm, dirty = card.item, false, true end)
                card.icon, card.icon_loaded = icon(card.obj, item, 10, 12)
                card.title = label(card.obj, "", 70, 10, w - 82)
                card.subtitle = label(card.obj, "", 70, 38, w - 82)
                cards[item.id] = card
            elseif not card.icon_loaded and not task and storage.exists(cache .. "/" .. item.id .. ".jpg") then
                -- Replace a placeholder only after the worker finishes writing icons.
                card.icon, card.icon_loaded = icon(card.obj, item, 10, 12, card.icon)
            end
            local title = compact(item.title, w - 82):gsub("×", "x")
            local subtitle = compact(state or item.description, w - 82):gsub("×", "x")
            local color = state and C.accent or C.muted
            if card.title_text ~= title then card.title:set_text(title); card.title_text = title end
            if card.subtitle_text ~= subtitle then card.subtitle:set_text(subtitle); card.subtitle_text = subtitle end
            if card.color ~= color then card.subtitle:set_style({ text_color = color }); card.color = color end
            if card.y ~= count * 80 then card.y = count * 80; card.obj:set_pos(0, card.y) end
            if card.hidden then card.obj:clear_flag("HIDDEN"); card.hidden = false end
            count = count + 1
        elseif card and not card.hidden then
            card.obj:add_flag("HIDDEN")
            card.hidden = true
        end
        if card then card.item = item end
    end
    for id, card in pairs(cards) do
        if not seen[id] then card.obj:delete(); cards[id] = nil end
    end
    if count == 0 then empty:clear_flag("HIDDEN") else empty:add_flag("HIDDEN") end
end

local function render_detail(w, h)
    local item, state = selected, installed(selected.id)
    if not detail_page then
        detail_page = lvgl.object(body, { x = 0, y = 0, w = w, h = h, bg_opa = 0, pad = 0, border_width = 0 })
        detail_page:clear_flag("SCROLLABLE")
        local detail = lvgl.object(detail_page, { x = 0, y = 0, w = w, h = h - 52, bg_opa = 0, pad = 0, border_width = 0 })
        detail:set_scroll({ dir = "ver", scrollbar = "auto" })
        label(detail, compact(item.title, w), 0, 0, w)
        detail_view = { content = detail }
        detail_view.icon, detail_view.icon_loaded = icon(detail, item, 0, 32)
        detail_view.state = label(detail, "", 62, 32, w - 62, 24, C.accent)
        detail_view.size = label(detail, string.format("%.0f KB", (tonumber(item.totalSize) or 0) / 1024), 62, 58, w - 62, 24, C.muted)
        detail_view.description = label(detail, "", 0, 94, w - 8, 48, C.muted)
        local peripherals = (item.metadata or {}).peripherals or {}
        if #peripherals > 0 then detail_view.peripherals = label(detail, "外设: " .. table.concat(peripherals, ", "), 0, 0, w - 8, 72) end
        detail_view.button = button(detail_page, "", 0, h - 44, w, function()
            -- Resolve the current state; installation can change it while this button survives.
            local current = installed(selected.id)
            if task or current == "内置" or selected.id == "app_store" then return end
            if not current then start("install", selected)
            elseif confirm then start("remove", selected)
            else confirm, dirty, notice = true, true, "再次点击按钮确认卸载" end
        end)
    end
    local view = detail_view
    -- Update only changed controls, retaining the detail page and decoded icon.
    local state_text = state or "可安装"
    if view.state_text ~= state_text then view.state:set_text(state_text); view.state_text = state_text end
    if not view.icon_loaded and not task and storage.exists(cache .. "/" .. item.id .. ".jpg") then
        view.icon, view.icon_loaded = icon(view.content, item, 0, 32, view.icon)
    end
    if view.confirm ~= confirm then
        local description = confirm and "确认卸载此应用？应用文件将被删除。再次点击下方按钮确认。" or item.description
        local text_height = math.max(48, math.ceil((utf8.len(description) or #description) / math.max(1, math.floor((w - 8) / 18))) * 24)
        view.description:set_text(tostring(description):gsub("×", "x"))
        view.description:set_pos(0, confirm and 32 or 94)
        view.description:set_size(w - 8, text_height)
        for _, obj in ipairs({ view.icon, view.state, view.size, view.peripherals }) do
            if confirm then obj:add_flag("HIDDEN") else obj:clear_flag("HIDDEN") end
        end
        if view.peripherals then view.peripherals:set_pos(0, 102 + text_height) end
        view.confirm = confirm
    end
    local protected = state == "内置" or item.id == "app_store"
    local title = task and "处理中…" or (protected and "内置 / 当前应用" or (state and (confirm and "确认卸载" or "卸载应用") or "安装应用"))
    local accent = not protected and not task and (state and C.danger or C.accent) or false
    if view.button_text ~= title then view.button:set_text(title); view.button_text = title end
    if view.accent ~= accent then
        view.button:set_style({ border_color = accent or C.line, bg_color = accent or C.card, text_color = accent and C.bg or C.text })
        view.accent = accent
    end
end

local function render()
    local w, h = width - 24, height - 100
    if selected then
        if home then home:add_flag("HIDDEN") end
        render_detail(w, h)
    else
        if detail_page then
            detail_page:delete()
            detail_page, detail_view = nil, nil
            -- Refresh the static header when leaving a scrolled detail page.
            screen:set_style({ bg_color = C.bg })
        end
        render_catalog(w, h)
        home:clear_flag("HIDDEN")
    end
end

local function failure(result)
    local message = result and result.error or "worker stopped"
    print("[AppStore] " .. message)
    if message:find("Cancelled", 1, true) then return "操作已取消" end
    if message:find("storage", 1, true) or message:find("write", 1, true) then return "存储不足或写入失败" end
    if message:find("installed", 1, true) then return "应用已安装，请刷新" end
    if message:find("allowlist", 1, true) or message:find("not allowed", 1, true) then return "请配置服务器访问白名单" end
    return "操作失败，请检查网络后重试"
end

local function run()
    lvgl.init()
    owned = true
    assert(not worker_active(), "App Store worker is still running; retry shortly")
    for _, path in ipairs({ root .. "/cache", cache }) do if not storage.exists(path) then storage.mkdir(path) end end
    while thread.sync.queue_recv(queue, 0) do end
    thread.sync.queue_delete(queue)
    assert(thread.sync.queue_create(queue, { depth = 8, item_size = 512 }))
    font = lvgl.font_load({ size = 18, cache_size = 96 })
    lvgl.set_default_font(font)
    screen = lvgl.create_screen()
    width, height = screen:get_size()
    assert(width >= 240 and height >= 240, "App Store needs a 240x240 or larger display")
    screen:set_style({ bg_color = C.bg, pad = 0 })
    screen:clear_flag("SCROLLABLE")
    button(screen, "返回", 12, 8, 56, function()
        if selected then selected, confirm, dirty = nil, false, true else exit() end
    end)
    label(screen, "应用商店", 78, 18, width - 156)
    button(screen, "刷新", width - 68, 8, 56, function() start("catalog") end)
    body = lvgl.object(screen, { x = 12, y = 56, w = width - 24, h = height - 100, bg_opa = 0, pad = 0, border_width = 0 })
    body:clear_flag("SCROLLABLE")
    status = label(screen, "", 12, height - 36, width - 24, 24, C.muted)
    -- Use plain objects to avoid the default theme's blue bar indicator.
    local track = lvgl.object(screen, { x = 12, y = height - 8, w = width - 24, h = 4, pad = 0, radius = 2, border_width = 0, bg_color = C.tint })
    track:clear_flag("SCROLLABLE")
    bar = lvgl.object(track, { x = 0, y = 0, w = 1, h = 4, radius = 2, border_width = 0, bg_color = C.accent })
    bar:clear_flag("SCROLLABLE")
    bar:add_flag("HIDDEN")
    screen:load()
    reload()
    start("catalog")
    local ticks = 0
    while running do
        lvgl.process_events(0)
        if dirty then dirty = false; render() end
        ticks = ticks + 1
        if ticks % 8 == 0 then
            while true do
                local message = thread.sync.queue_recv(queue, 0)
                if not message then break end
                local p = json.decode(message)
                if not closing then notice, percent = p.text, p.value end
            end
            -- Wait for termination before another job can reuse the staging directory.
            if task and not worker_active() then
                local result = read(cache .. "/result.json")
                notice = result and result.ok and (task == "install" and "已安装，返回桌面启动" or "操作完成") or failure(result)
                task, percent = nil, 0
                reload()
                if closing then running = false end
            end
            status:set_text(compact(notice, width - 24))
            if percent <= 0 then bar:add_flag("HIDDEN")
            else
                bar:set_size(math.max(1, math.floor((width - 24) * math.min(100, percent) / 100)), 4)
                bar:clear_flag("HIDDEN")
            end
        end
        delay.delay_ms(30)
    end
end

local ok, err = xpcall(run, debug.traceback)
if task then pcall(storage.write_file, cache .. "/cancel", "1") end
if owned then lvgl.deinit() end
if not worker_active() then
    pcall(function()
        while thread.sync.queue_recv(queue, 0) do end
        thread.sync.queue_delete(queue)
    end)
end
if not ok then error(err) end
