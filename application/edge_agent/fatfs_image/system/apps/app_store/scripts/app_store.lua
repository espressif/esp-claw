local lvgl, storage, json = require("lvgl"), require("storage"), require("json")
local thread, image, delay = require("thread"), require("image"), require("delay")
local scripts = assert(debug.getinfo(1, "S").source:match("^@(.*)/[^/]+$"))
local root = storage.get_root_dir()
local cache = storage.join_path(root, "cache", "app_store")
local settings_key = "app_store"
local default_server = ((args or {}).server or "https://skills-lab.esp-claw.com"):gsub("/+$", "")
local job, queue = "app_store_worker", "app_store_progress"
local items, selected, task = {}, nil, nil
local running, closing, owned = true, false, false
local language, server, server_draft, tab = "en", default_server, default_server, 1
local notice, percent, page, confirm, settings_confirm = "", 0, "catalog", false, nil
local screen, status, progress, width, height, font, title_font, M
local url_editor, dirty = nil, true
local C = {
    bg = "#0b0e10", panel = "#121619", surface = "#1b2025", selected = "#202b27",
    text = "#f5f7f8", muted = "#929ba5", accent = "#43e98b", danger = "#ff5c65", line = "#2a3137"
}
local T = {
    en = {
        preparing = "Preparing app catalog", busy = "Task in progress. Please wait.", start_failed = "Could not start task", connecting = "Connecting to server",
        cancelling = "Cancelling. Please wait.", all = "All", games = "Games", tools = "Tools", local_apps = "Local", empty = "No apps yet. Tap Refresh.",
        local_app = "Local app", builtin = "Built-in", installed = "Installed", available = "Available",
        peripherals = "Peripherals: ", uninstall_hint = "Tap again to uninstall", uninstall_confirm = "Uninstall this app? Its files will be deleted.", working = "Working...",
        protected = "Built-in / Current app", confirm_uninstall = "Confirm uninstall", uninstall = "Uninstall app", install = "Install app", cancelled = "Operation cancelled",
        storage_failed = "Storage full or write failed", already_installed = "App installed. Tap Refresh.", allowlist = "Add server to HTTP allowlist",
        failed = "Failed. Check network and retry.", app_store = "App Store", settings = "Settings",
        installed_done = "Installed. Open from Launcher.", done = "Done", language = "Language", server = "Server URL", save = "Save changes",
        clear_cache = "Clear cache", clear_apps = "Clear apps", confirm_cache = "Tap Clear cache again", confirm_apps = "Tap Clear apps again",
        cache_cleared = "Cache cleared", apps_cleared = "Apps cleared", saved = "Settings saved", invalid_url = "Use http(s)://host[:port]",
        clear_failed = "Could not clear data", confirm = "Confirm", size = "Size", ready = "Ready"
    },
    zh = {
        preparing = "正在准备应用目录", busy = "任务进行中，请稍候", start_failed = "任务启动失败", connecting = "正在连接服务器", cancelling = "正在取消，请稍候",
        all = "全部", games = "游戏", tools = "工具", local_apps = "已装",
        empty = "暂无应用，请点击刷新", local_app = "本地应用", builtin = "内置", installed = "已安装", available = "可安装", peripherals = "外设：",
        uninstall_hint = "再次点击确认卸载", uninstall_confirm = "确认卸载此应用？应用文件将被删除。", working = "处理中…", protected = "内置 / 当前应用",
        confirm_uninstall = "确认卸载", uninstall = "卸载应用", install = "安装应用", cancelled = "操作已取消", storage_failed = "存储不足或写入失败",
        already_installed = "应用已安装，请刷新", allowlist = "请将服务器加入 HTTP 白名单", failed = "操作失败，请检查网络",
        app_store = "应用商店", settings = "设置", installed_done = "安装完成，请从桌面启动", done = "操作完成", language = "语言",
        server = "服务器 URL", save = "保存更改", clear_cache = "清空缓存", clear_apps = "清空应用", confirm_cache = "再次点击清空缓存",
        confirm_apps = "再次点击清空应用", cache_cleared = "缓存已清空", apps_cleared = "应用已清空", saved = "设置已保存",
        invalid_url = "请输入 http(s)://主机[:端口]", clear_failed = "清理失败", confirm = "确认", size = "大小", ready = "就绪"
    }
}

local function t(key)
    return T[language][key] or key
end

local function read(path)
    local ok, value = pcall(function() return json.decode(storage.read_file(path)) end)
    return ok and type(value) == "table" and value or nil
end

local function normalize_server(value)
    value = type(value) == "string" and value:match("^%s*(.-)%s*$") or ""
    value = value:gsub("/+$", "")
    return value:match("^https?://[^/%s%?#]+$") and value or nil
end

local function load_settings()
    local ok, raw = pcall(storage.get_app_setting, settings_key, "{}")
    local value = {}
    if ok then
        local decoded, stored = pcall(json.decode, raw)
        if decoded and type(stored) == "table" then value = stored end
    else
        print("[AppStore] load settings: " .. tostring(raw))
    end
    language = value.language == "zh" and "zh" or "en"
    server = normalize_server(value.server) or default_server
    server_draft, notice = server, t("preparing")
end

local function save_settings()
    storage.set_app_setting(settings_key, json.encode({ language = language, server = server }))
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
    if storage.exists("/system/apps/" .. id .. "/launcher.json") then return "builtin" end
    if storage.exists(root .. "/apps/" .. id .. "/launcher.json") then return "installed" end
end

local function reload()
    items = read(cache .. "/catalog.json") or {}
    local seen = {}
    for _, item in ipairs(items) do seen[item.id] = true end
    for _, base in ipairs({ root .. "/apps", "/system/apps" }) do
        if storage.exists(base) then
            for _, entry in ipairs(storage.listdir(base)) do
                if entry.type == "dir" and not seen[entry.name] then
                    local manifest = read(base .. "/" .. entry.name .. "/launcher.json")
                    if manifest and manifest.id == entry.name then
                        items[#items + 1] = { id = manifest.id, title = manifest.display_name or manifest.id, description = (manifest.catalog or {}).description or t("local_app"), metadata = manifest.catalog or {} }
                        seen[manifest.id] = true
                    end
                end
            end
        end
    end
    dirty = true
end

local function compact(text, pixels, unit)
    text, unit = tostring(text):gsub("%s+", " "), unit or 9
    local used, last = 0, 0
    for pos, cp in utf8.codes(text) do
        used = used + (cp < 128 and unit or math.floor(unit * 1.7))
        if used > pixels then return text:sub(1, last) .. "…" end
        if used <= pixels - unit * 2 then last = (utf8.offset(text, 2, pos) or (#text + 1)) - 1 end
    end
    return text
end

local function label(parent, text, x, y, w, h, color, large)
    return lvgl.label(parent, { text = tostring(text):gsub("×", "x"), x = x, y = y, w = w, h = h or M.text_h, text_color = color or C.text, font = large and title_font or font })
end

local function box(parent, x, y, w, h, color, radius)
    local obj = lvgl.object(parent, { x = x, y = y, w = w, h = h, pad = 0, radius = radius or 0, border_width = 0, bg_color = color or C.panel })
    obj:clear_flag("SCROLLABLE")
    return obj
end

local function line(parent, x, y, w)
    return box(parent, x, y, math.max(1, w), 1, C.line)
end

local function vline(parent, x, y, h)
    return box(parent, x, y, 1, math.max(1, h), C.line)
end

local function button(parent, text, x, y, w, action, kind, h)
    local fill = kind == "accent" and C.accent or kind == "danger" and C.danger or C.surface
    local color = (kind == "accent" or kind == "danger") and C.bg or C.text
    local b = lvgl.button(parent, { text = text, x = x, y = y, w = w, h = h or M.button_h, radius = M.radius, pad = 0, border_width = 0, bg_color = fill, text_color = color, font = font })
    b:on("clicked", action)
    return b
end

local function icon_button(parent, icon, x, y, w, action, kind, h)
    h = h or M.button_h
    local b = button(parent, "", x, y, w, action, kind, h)
    local scale = h <= 30 and 0.72 or 1
    local ox, oy = math.floor((w - 24 * scale) / 2), math.floor((h - 24 * scale) / 2)
    local color = (kind == "accent" or kind == "danger") and C.bg or C.text
    local function points(values)
        local result = {}
        for _, value in ipairs(values) do result[#result + 1] = { x = math.floor(ox + value[1] * scale), y = math.floor(oy + value[2] * scale) } end
        return result
    end
    local function stroke(values)
        return lvgl.line(b, { points = points(values), line_color = color, line_width = 2 })
    end
    if icon == "back" then
        stroke({ { 16, 5 }, { 7, 12 }, { 16, 19 } })
        stroke({ { 7, 12 }, { 21, 12 } })
    elseif icon == "refresh" then
        stroke({ { 19, 7 }, { 14, 5 }, { 9, 7 }, { 6, 11 }, { 6, 16 }, { 9, 20 }, { 14, 21 }, { 19, 18 } })
        stroke({ { 18, 3 }, { 19, 7 }, { 15, 8 } })
    else
        stroke({ { 5, 7 }, { 19, 7 } })
        stroke({ { 5, 12 }, { 19, 12 } })
        stroke({ { 5, 17 }, { 19, 17 } })
        stroke({ { 9, 4 }, { 9, 10 } })
        stroke({ { 15, 9 }, { 15, 15 } })
        stroke({ { 11, 14 }, { 11, 20 } })
    end
    return b
end

local function app_icon(parent, item, x, y, size)
    local holder = box(parent, x, y, size, size, C.surface, M.radius)
    local path = cache .. "/" .. item.id .. ".jpg"
    if not storage.exists(path) then
        label(holder, "APP", 0, math.floor((size - M.text_h) / 2), size, M.text_h, C.muted)
        return holder
    end
    local frames = {}
    local ok, err = pcall(function()
        frames[1] = image.load_file(path)
        frames[2] = image.resize(frames[1], { width = size, height = size })
        local view = lvgl.image(holder, { x = 0, y = 0 })
        view:set_raw_rgb565(size, size, frames[2]:data())
    end)
    for _, frame in ipairs(frames) do frame:release() end
    if not ok then print("[AppStore] image: " .. tostring(err)); label(holder, "APP", 0, math.floor((size - M.text_h) / 2), size, M.text_h, C.muted) end
    return holder
end

local function start(action, item)
    if task or worker_active() then notice = t("busy"); return end
    erase(cache .. "/result.json")
    erase(cache .. "/cancel")
    while thread.sync.queue_recv(queue, 0) do end
    local ok, err = thread.start(scripts .. "/store_worker.lua", { action = action, id = item and item.id, size = item and item.totalSize, server = server, language = language, queue = queue }, { name = job, exclusive = job, replace = false, timeout_ms = 0 })
    if not ok then print("[AppStore] start: " .. tostring(err)); notice = t("start_failed"); return end
    task, notice, percent, confirm, settings_confirm, dirty = action, t("connecting"), 0, false, nil, true
end

local function exit()
    if task or worker_active() then
        storage.write_file(cache .. "/cancel", "1")
        closing, notice = true, t("cancelling")
    else
        running = false
    end
end

local function categories(item)
    return "," .. table.concat((item.metadata or {}).category or {}, ",") .. ","
end

local function visible_items()
    local result = {}
    for _, item in ipairs(items) do
        local category, state = categories(item), installed(item.id)
        if tab == 1 or (tab == 2 and category:find(",game,", 1, true)) or (tab == 3 and category:find(",utility,", 1, true)) or (tab == 4 and state) then result[#result + 1] = item end
    end
    return result
end

local function choose_item(item)
    selected, confirm = item, false
    if M.compact then page = "detail" end
    dirty = true
end

local function render_list(parent, x, y, w, h)
    local list = lvgl.object(parent, { x = x, y = y, w = w, h = h, pad = 0, border_width = 0, bg_opa = 0 })
    list:set_scroll({ dir = "ver", scrollbar = "auto" })
    local filtered = visible_items()
    if #filtered == 0 then label(list, t("empty"), M.pad, M.pad, w - M.pad * 2, M.text_h * 3, C.muted); return end
    for index, item in ipairs(filtered) do
        local current, row_y = item, (index - 1) * M.row_h
        local row = box(list, 0, row_y, w - 2, M.row_h, selected and selected.id == item.id and C.selected or C.panel)
        row:add_flag("CLICKABLE")
        row:on("clicked", function() choose_item(current) end)
        app_icon(row, item, M.gap, math.floor((M.row_h - M.icon) / 2), M.icon)
        local text_x, text_w = M.gap * 2 + M.icon, w - M.gap * 3 - M.icon
        local state = installed(item.id)
        label(row, compact(item.title, text_w, M.font_unit), text_x, M.row_title_y, text_w, M.text_h, C.text)
        label(row, compact(state and t(state) or item.description, text_w, M.font_unit), text_x, M.row_sub_y, text_w, M.text_h, state and C.accent or C.muted)
        line(list, M.gap, row_y + M.row_h - 1, w - M.gap * 2)
    end
end

local function action_for(item)
    local state = installed(item.id)
    local protected = state == "builtin" or item.id == "app_store"
    if task then return t("working"), nil end
    if protected then return t("protected"), nil end
    if not state then return t("install"), "accent" end
    return confirm and t("confirm_uninstall") or t("uninstall"), "danger"
end

local function perform_item_action()
    local state = selected and installed(selected.id)
    if not selected or task or state == "builtin" or selected.id == "app_store" then return end
    if not state then
        start("install", selected)
    elseif confirm then
        start("remove", selected)
    else
        confirm, notice, dirty = true, t("uninstall_hint"), true
    end
end

local function render_detail(parent, x, y, w, h)
    if not selected then return end
    local item, state = selected, installed(selected.id)
    local action_h = M.button_h
    local scroll = lvgl.object(parent, { x = x, y = y, w = w, h = h - action_h - M.gap, pad = 0, border_width = 0, bg_opa = 0 })
    scroll:set_scroll({ dir = "ver", scrollbar = "auto" })
    local top = M.pad
    app_icon(scroll, item, M.pad, top, M.detail_icon)
    local info_x = M.pad * 2 + M.detail_icon
    label(scroll, compact(item.title, w - info_x - M.pad, M.title_unit), info_x, top, w - info_x - M.pad, M.title_h, C.text, true)
    label(scroll, state and t(state) or t("available"), info_x, top + M.title_h + 2, w - info_x - M.pad, M.text_h, state and C.accent or C.muted)
    label(scroll, string.format("%s  %.0f KB", t("size"), (tonumber(item.totalSize) or 0) / 1024), info_x, top + M.title_h + M.text_h + 4, w - info_x - M.pad, M.text_h, C.muted)
    local description = confirm and t("uninstall_confirm") or item.description or ""
    local desc_y = top + M.detail_icon + M.gap * 2
    local chars = math.max(1, math.floor((w - M.pad * 2) / math.max(8, M.font_unit)))
    local desc_h = math.max(M.text_h * 3, math.ceil((utf8.len(description) or #description) / chars) * M.text_h)
    label(scroll, description, M.pad, desc_y, w - M.pad * 2, desc_h, confirm and C.danger or C.muted)
    local peripherals = (item.metadata or {}).peripherals or {}
    if #peripherals > 0 then label(scroll, t("peripherals") .. table.concat(peripherals, ", "), M.pad, desc_y + desc_h + M.gap, w - M.pad * 2, M.text_h * 2, C.text) end
    local text, kind = action_for(item)
    button(parent, text, x + M.pad, y + h - action_h, w - M.pad * 2, perform_item_action, kind, action_h)
end

local function open_url_editor()
    if url_editor or task then return end
    local overlay = box(screen, 0, 0, width, height, C.bg)
    local input_h = M.button_h
    local back_w = M.tiny and 28 or M.button_h
    local input_x = M.pad + back_w + M.gap
    local input = lvgl.textarea(overlay, { text = server_draft, x = input_x, y = M.pad, w = width - input_x - M.pad, h = input_h, one_line = true, max_length = 160, font = font, bg_color = C.surface, text_color = C.text, border_color = C.line, border_width = 1, radius = M.radius })
    local keyboard = lvgl.keyboard(overlay, { x = 0, y = M.pad * 2 + input_h, w = width, h = height - input_h - M.pad * 2, mode = "text_lower", textarea = input, font = font })
    url_editor = { root = overlay, input = input }
    local function close(commit)
        if not url_editor then return end
        if commit then server_draft = input:get_text() end
        url_editor.root:delete()
        url_editor = nil
        dirty = true
    end
    icon_button(overlay, "back", M.pad, M.pad, back_w, function() close(false) end, nil, input_h)
    keyboard:on("ready", function() close(true) end)
    keyboard:on("cancel", function() close(false) end)
end

local function clear_cache()
    local ok, err = storage.remove_tree(cache)
    if not ok then print("[AppStore] clear cache: " .. tostring(err.message)); notice = t("clear_failed"); return end
    storage.mkdir(cache)
    reload()
    notice, settings_confirm = t("cache_cleared"), nil
end

local function change_language(next_language)
    if next_language == language then return end
    language = next_language
    local ok, err = pcall(save_settings)
    if not ok then print("[AppStore] save language: " .. tostring(err)) end
    notice = ok and t("saved") or t("storage_failed")
    reload()
end

local function render_settings(parent, x, y, w, h)
    local view_w = math.min(w, 560)
    local view_x = x + math.floor((w - view_w) / 2)
    local form = lvgl.object(parent, { x = view_x, y = y, w = view_w, h = h, pad = 0, border_width = 0, bg_opa = 0 })
    form:set_scroll({ dir = "ver", scrollbar = "auto" })
    local inner_w, cursor = view_w - M.pad * 2, M.pad
    label(form, t("language"), M.pad, cursor, inner_w, M.text_h, C.muted)
    cursor = cursor + M.text_h + M.gap
    local language_select = lvgl.dropdown(form, { x = M.pad, y = cursor, w = inner_w, h = M.button_h, options = { "English", "中文" }, selected = language == "zh" and 2 or 1, symbol = " ", font = font, bg_color = C.surface, text_color = C.text, border_color = C.line, border_width = 1, radius = M.radius })
    language_select:on("value_changed", function() change_language(language_select:get_value() == 2 and "zh" or "en") end)
    cursor = cursor + M.button_h + M.gap * 2
    label(form, t("server"), M.pad, cursor, inner_w, M.text_h, C.muted)
    cursor = cursor + M.text_h + M.gap
    if M.tiny then
        local input = box(form, M.pad, cursor, inner_w, M.button_h, C.surface, M.radius)
        input:add_flag("CLICKABLE")
        input:on("clicked", open_url_editor)
        label(input, compact(server_draft, inner_w - M.gap * 2, M.font_unit), M.gap, math.floor((M.button_h - M.text_h) / 2), inner_w - M.gap * 2, M.text_h, C.text)
    else
        local input = lvgl.textarea(form, { text = server_draft, x = M.pad, y = cursor, w = inner_w, h = M.button_h, one_line = true, max_length = 160, font = font, bg_color = C.surface, text_color = C.text, border_color = C.line, border_width = 1, radius = M.radius })
        input:on("clicked", open_url_editor)
    end
    cursor = cursor + M.button_h + M.gap * 2
    local stacked = inner_w < 330
    local danger_w = stacked and inner_w or math.floor((inner_w - M.gap) / 2)
    local cache_text = settings_confirm == "cache" and t("confirm") or t("clear_cache")
    local apps_text = settings_confirm == "apps" and t("confirm") or t("clear_apps")
    button(form, cache_text, M.pad, cursor, danger_w, function()
        if task then notice = t("busy"); return end
        if settings_confirm == "cache" then clear_cache() else settings_confirm, notice, dirty = "cache", t("confirm_cache"), true end
    end, "danger")
    local apps_y = stacked and cursor + M.button_h + M.gap or cursor
    local apps_x = stacked and M.pad or M.pad + danger_w + M.gap
    button(form, apps_text, apps_x, apps_y, danger_w, function()
        if task then notice = t("busy"); return end
        if settings_confirm == "apps" then start("clear_apps") else settings_confirm, notice, dirty = "apps", t("confirm_apps"), true end
    end, "danger")
    cursor = apps_y + M.button_h + M.gap * 2
    button(form, t("save"), M.pad, cursor, inner_w, function()
        local value = normalize_server(server_draft)
        if not value then notice = t("invalid_url"); dirty = true; return end
        server = value
        local ok, err = pcall(save_settings)
        if not ok then print("[AppStore] save settings: " .. tostring(err)); notice = t("storage_failed"); dirty = true; return end
        server_draft, settings_confirm, notice, dirty = server, nil, t("saved"), true
    end, "accent")
end

local function go_back()
    if page == "catalog" then exit() else page, selected, confirm, settings_confirm, dirty = "catalog", nil, false, nil, true end
end

local function switch_tab(index)
    tab, page, selected, confirm, dirty = index, "catalog", nil, false, true
end

local function layout()
    local compact_mode = width < 560 or height < 540
    local tiny = width < 180
    local pad = tiny and 5 or compact_mode and 8 or 16
    local text_h = tiny and 18 or 22
    return {
        compact = compact_mode, tiny = tiny, pad = tiny and 4 or pad, gap = tiny and 3 or 8, radius = tiny and 4 or 7,
        header_h = tiny and 34 or compact_mode and 48 or 64, status_h = tiny and 20 or 30, button_h = tiny and 28 or 40,
        tab_h = tiny and 26 or 38, text_h = tiny and 15 or text_h, title_h = tiny and 18 or compact_mode and 28 or 36,
        row_h = tiny and 46 or compact_mode and 66 or 74, icon = tiny and 30 or compact_mode and 44 or 50,
        detail_icon = tiny and 38 or compact_mode and 58 or 72, font_unit = tiny and 6 or 9, title_unit = tiny and 8 or 14,
        row_title_y = tiny and 4 or 9, row_sub_y = tiny and 23 or compact_mode and 36 or 41,
        sidebar_w = compact_mode and 0 or math.max(156, math.min(220, math.floor(width * 0.22)))
    }
end

local function render_compact()
    local content_y, content_h = M.header_h, height - M.header_h - M.status_h
    local back_w = M.tiny and 28 or 54
    icon_button(screen, "back", M.pad, math.floor((M.header_h - M.button_h) / 2), back_w, go_back)
    local right_w = M.button_h * 2 + M.gap
    local title_x, title_w = M.pad + back_w + M.gap, width - back_w - right_w - M.pad * 3 - M.gap
    if not M.tiny and title_w >= 20 then label(screen, compact(page == "settings" and t("settings") or page == "detail" and (selected and selected.title or t("app_store")) or t("app_store"), title_w, M.title_unit), title_x, math.floor((M.header_h - M.title_h) / 2), title_w, M.title_h, C.text, true) end
    if page ~= "settings" then
        icon_button(screen, "refresh", width - M.pad - right_w, math.floor((M.header_h - M.button_h) / 2), M.button_h, function() start("catalog") end)
        icon_button(screen, "settings", width - M.pad - M.button_h, math.floor((M.header_h - M.button_h) / 2), M.button_h, function() page, settings_confirm, dirty = "settings", nil, true end)
    end
    line(screen, 0, M.header_h - 1, width)
    if page == "catalog" then
        local names = M.tiny and (language == "zh" and { "全", "游", "工", "装" } or { "A", "G", "T", "L" }) or { t("all"), t("games"), t("tools"), t("local_apps") }
        local tab_w = math.floor(width / 4)
        for index, name in ipairs(names) do
            local current = index
            local b = button(screen, name, (index - 1) * tab_w, content_y, index == 4 and width - tab_w * 3 or tab_w, function() switch_tab(current) end, index == tab and "accent" or nil, M.tab_h)
            b:set_style({ radius = 0 })
        end
        render_list(screen, 0, content_y + M.tab_h, width, content_h - M.tab_h)
    elseif page == "detail" then
        render_detail(screen, 0, content_y, width, content_h)
    else
        render_settings(screen, 0, content_y, width, content_h)
    end
end

local function render_wide()
    local content_y = M.header_h
    local content_h = height - M.header_h - M.status_h
    local header = box(screen, 0, 0, width, M.header_h, C.bg)
    icon_button(header, "back", M.pad, math.floor((M.header_h - M.button_h) / 2), M.button_h, go_back)
    local brand_x = M.pad + M.button_h + M.gap
    label(header, t("app_store"), brand_x, math.floor((M.header_h - M.text_h) / 2), M.sidebar_w - brand_x - M.gap, M.text_h, C.text)
    vline(header, M.sidebar_w - 1, 0, M.header_h)
    local page_title = page == "settings" and t("settings") or page == "detail" and selected and selected.title or ({ t("all"), t("games"), t("tools"), t("local_apps") })[tab]
    label(header, page_title, M.sidebar_w + M.pad, math.floor((M.header_h - M.title_h) / 2), width - M.sidebar_w - M.pad * 3 - M.button_h * 2 - M.gap, M.title_h, C.text, true)
    if page ~= "settings" then
        local action_y = math.floor((M.header_h - M.button_h) / 2)
        icon_button(header, "refresh", width - M.pad * 2 - M.button_h * 2 - M.gap, action_y, M.button_h, function() start("catalog") end)
        icon_button(header, "settings", width - M.pad - M.button_h, action_y, M.button_h, function() page, settings_confirm, dirty = "settings", nil, true end)
    end
    line(screen, 0, M.header_h - 1, width)
    local sidebar = box(screen, 0, content_y, M.sidebar_w, content_h, C.panel)
    local y = M.pad
    for index, name in ipairs({ t("all"), t("games"), t("tools"), t("local_apps") }) do
        local current = index
        button(sidebar, name, M.pad, y, M.sidebar_w - M.pad * 2, function() switch_tab(current) end, page == "catalog" and tab == index and "accent" or nil)
        y = y + M.button_h + M.gap
    end
    vline(screen, M.sidebar_w - 1, content_y, content_h)
    local x, w = M.sidebar_w, width - M.sidebar_w
    if page == "settings" then
        render_settings(screen, x, content_y, w, content_h)
    elseif page == "detail" then
        render_detail(screen, x, content_y, w, content_h)
    else
        local filtered = visible_items()
        if not selected then selected = filtered[1] end
        local list_w = math.floor(w * 0.46)
        render_list(screen, x, content_y, list_w, content_h)
        vline(screen, x + list_w, content_y, content_h)
        if selected then render_detail(screen, x + list_w + 1, content_y, w - list_w - 1, content_h) end
    end
end

local function render()
    screen:clean()
    M = layout()
    if M.compact then render_compact() else render_wide() end
    local status_y = height - M.status_h
    box(screen, 0, status_y, width, M.status_h, C.bg)
    line(screen, 0, status_y, width)
    status = label(screen, compact(notice ~= "" and notice or t("ready"), width - M.pad * 2, M.font_unit), M.pad, status_y + math.floor((M.status_h - M.text_h) / 2), width - M.pad * 2, M.text_h, C.muted)
    progress = box(screen, 0, status_y, math.max(1, math.floor(width * math.min(100, percent) / 100)), 3, C.accent)
    if percent <= 0 then progress:add_flag("HIDDEN") end
end

local function failure(result)
    local message = result and result.error or "worker stopped"
    print("[AppStore] " .. message)
    if message:find("Cancelled", 1, true) then return t("cancelled") end
    if message:find("storage", 1, true) or message:find("write", 1, true) then return t("storage_failed") end
    if message:find("installed", 1, true) then return t("already_installed") end
    if message:find("allowlist", 1, true) or message:find("not allowed", 1, true) then return t("allowlist") end
    return t("failed")
end

local function update_status()
    if not status then return end
    status:set_text(compact(notice ~= "" and notice or t("ready"), width - M.pad * 2, M.font_unit))
    if percent <= 0 then
        progress:add_flag("HIDDEN")
    else
        progress:set_size(math.max(1, math.floor(width * math.min(100, percent) / 100)), 3)
        progress:clear_flag("HIDDEN")
    end
end

local function run()
    lvgl.init()
    owned = true
    load_settings()
    assert(not worker_active(), "App Store worker is still running; retry shortly")
    for _, path in ipairs({ root .. "/cache", cache }) do if not storage.exists(path) then storage.mkdir(path) end end
    while thread.sync.queue_recv(queue, 0) do end
    thread.sync.queue_delete(queue)
    assert(thread.sync.queue_create(queue, { depth = 8, item_size = 512 }))
    screen = lvgl.create_screen()
    width, height = screen:get_size()
    assert(width >= 125 and height >= 240, "App Store needs a 125x240 or larger display")
    local compact_mode = width < 560 or height < 540
    font = lvgl.font_load({ size = width < 180 and 11 or compact_mode and 16 or 18, cache_size = 96 })
    title_font = lvgl.font_load({ size = width < 180 and 14 or compact_mode and 21 or 28, cache_size = 48 })
    lvgl.set_default_font(font)
    screen:set_style({ bg_color = C.bg, pad = 0 })
    screen:clear_flag("SCROLLABLE")
    screen:load()
    reload()
    start("catalog")
    local ticks = 0
    while running do
        lvgl.process_events(0)
        if dirty and not url_editor then dirty = false; render() end
        ticks = ticks + 1
        if ticks % 8 == 0 then
            while true do
                local message = thread.sync.queue_recv(queue, 0)
                if not message then break end
                local payload = json.decode(message)
                if not closing then notice, percent = payload.text, payload.value end
            end
            if task and not worker_active() then
                local result = read(cache .. "/result.json")
                notice = result and result.ok and (task == "install" and t("installed_done") or task == "clear_apps" and t("apps_cleared") or t("done")) or failure(result)
                task, percent = nil, 0
                reload()
                if closing then running = false end
            end
            update_status()
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
