local storage = require("storage")
local json = require("json")
local capability = require("capability")
local thread = require("thread")
local a = assert(args, "missing store task")
local root = storage.get_root_dir()
local cache = storage.join_path(root, "cache", "app_store")
local stage = cache .. "/staging"
local installed_path

local function mkdir(path)
    local current = ""
    for part in path:gmatch("[^/]+") do
        current = current .. "/" .. part
        if not storage.exists(current) then storage.mkdir(current) end
    end
end

local function remove_tree(path)
    local info = storage.stat(path)
    if not info then return end
    if info.type == "dir" then
        for _, entry in ipairs(storage.listdir(path)) do remove_tree(path .. "/" .. entry.name) end
    end
    storage.remove(path)
end

local function valid_id(id)
    return type(id) == "string" and #id <= 63 and id:match("^[A-Za-z0-9_-]+$")
end

local function valid_path(path)
    if type(path) ~= "string" or path == "" or path:find("[%c\\:]") or path:sub(1, 1) == "/" or path:sub(-1) == "/" or path:find("//", 1, true) then return false end
    for part in path:gmatch("[^/]+") do if part == "." or part == ".." then return false end end
    return true
end

local function encode(path)
    return (path:gsub("[^%w%-%._~/]", function(char) return string.format("%%%02X", char:byte()) end))
end

local function cancelled()
    if storage.exists(cache .. "/cancel") then error("Cancelled") end
end

local function progress(text, value)
    thread.sync.queue_send(a.queue, json.encode({ text = text, value = value or 0 }), 0)
end

local function call(name, payload)
    local ok, out, err = capability.call(name, payload)
    if not ok then error(tostring(err or out or name .. " failed")) end
    return out
end

local function fetch(path, target, limit)
    cancelled()
    local request = { url = a.server .. path, method = "GET", timeout_ms = 20000 }
    if target then request.save_path, request.max_file_bytes = target, limit or 10 * 1024 * 1024
    else request.max_body_bytes = 60000 end
    local out = call("http_request", request)
    if not out:match("^HTTP 200[\r\n ]") or (out:match("^[^\n]*") or ""):find("truncated", 1, true) then
        if target and storage.exists(target) then storage.remove(target) end
        error("Download failed: " .. path .. " (" .. (out:match("^[^\n]*") or "HTTP error") .. ")")
    end
    cancelled()
    if not target then return json.decode(assert(out:match("^[^\n]*\n(.*)$"), "missing response body")) end
end

local function refresh()
    progress("正在获取应用目录")
    local entries = fetch("/raw/apps-data.json")
    assert(type(entries) == "table", "invalid catalog")
    local list, seen = {}, {}
    for _, item in ipairs(entries) do
        if type(item) == "table" and item.kind == "app" and valid_id(item.id) and not seen[item.id] then
            assert(type(item.title or item.name or item.id) == "string" and type(item.description or "") == "string", "invalid catalog text")
            assert(item.metadata == nil or type(item.metadata) == "table", "invalid catalog metadata")
            for _, field in ipairs({ "category", "peripherals" }) do
                local values = (item.metadata or {})[field] or {}
                assert(type(values) == "table", "invalid catalog tags")
                for _, value in ipairs(values) do assert(type(value) == "string", "invalid catalog tag") end
            end
            seen[item.id] = true
            list[#list + 1] = { id = item.id, title = item.title or item.name or item.id, description = item.description or "", metadata = item.metadata or {}, totalSize = item.totalSize, icon = item.icon }
        end
    end
    assert(#list > 0, "No App packages in catalog")
    storage.write_file(cache .. "/catalog.tmp", json.encode(list))
    if storage.exists(cache .. "/catalog.json") then storage.remove(cache .. "/catalog.json") end
    storage.rename(cache .. "/catalog.tmp", cache .. "/catalog.json")
    -- Icons are optional; a missing image must not hide the catalog.
    for i, item in ipairs(list) do
        cancelled()
        local target = cache .. "/" .. item.id .. ".jpg"
        if valid_path(item.icon) and not storage.exists(target) then
            progress("缓存图标 " .. i .. "/" .. #list)
            local ok, err = pcall(fetch, "/raw/apps/" .. item.id .. "/" .. encode(item.icon), target, 65536)
            if not ok then print("[AppStore] icon " .. item.id .. ": " .. tostring(err)) end
        end
    end
    cancelled()
end

local function install()
    assert(valid_id(a.id) and a.id ~= "app_store", "invalid App id")
    local target = storage.join_path(root, "apps", a.id)
    assert(not storage.exists(target) and not storage.exists("/system/apps/" .. a.id), "App already installed")
    progress("检查应用文件")
    local meta = fetch("/raw/apps/" .. a.id .. "/_metadata.json")
    assert(type(meta) == "table" and meta.kind == "app" and meta.name == a.id, "App metadata mismatch")
    assert(type(meta.files) == "table" and #meta.files > 0 and #meta.files <= 512, "invalid file list")
    local seen, count = {}, 0
    for key, path in pairs(meta.files) do
        assert(type(key) == "number" and key % 1 == 0 and key >= 1 and key <= #meta.files, "invalid file list")
        assert(valid_path(path) and not seen[path], "unsafe or duplicate file path")
        seen[path], count = true, count + 1
    end
    assert(count == #meta.files and seen["launcher.json"], "missing launcher.json")
    remove_tree(stage)
    local free = storage.get_free_space().free
    assert(free > (tonumber(a.size) or 0) + 65536, "Not enough storage")
    mkdir(stage)
    for i, path in ipairs(meta.files) do
        progress("下载 " .. i .. "/" .. count, math.floor((i - 1) * 100 / count))
        local file = stage .. "/" .. path
        mkdir(assert(file:match("^(.+)/[^/]+$")))
        fetch("/raw/apps/" .. a.id .. "/" .. encode(path), file)
    end
    local manifest = json.decode(storage.read_file(stage .. "/launcher.json"))
    assert(manifest.schema_version == 1 and manifest.id == a.id and valid_path(manifest.entry) and manifest.entry:match("%.lua$") and seen[manifest.entry], "invalid launcher")
    assert(manifest.icon == nil or (valid_path(manifest.icon) and seen[manifest.icon]), "invalid launcher icon")
    cancelled()
    mkdir(root .. "/apps")
    assert(not storage.exists(target), "App appeared during download")
    storage.rename(stage, target)
    installed_path = target
    progress("正在发布应用", 100)
    local result = json.decode(call("publish_app", { app_id = a.id }))
    assert(result.ok == true, "App publication failed")
    installed_path = nil
end

local function run()
    assert(type(a.server) == "string" and a.server:match("^https?://[^/]+$"), "invalid store server")
    mkdir(cache)
    if a.action == "catalog" then refresh()
    elseif a.action == "install" then install()
    elseif a.action == "remove" then
        assert(valid_id(a.id) and a.id ~= "app_store", "invalid App id")
        assert(not storage.exists("/system/apps/" .. a.id), "Built-in App cannot be removed")
        cancelled()
        assert(json.decode(call("remove_app", { app_id = a.id })).ok == true, "App removal failed")
    else error("unknown store task") end
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    print("[AppStore] " .. tostring(err))
    -- Only directories owned by this installation may be rolled back.
    local cleaned, cleanup_err = pcall(function()
        if a.action == "install" then remove_tree(stage) end
        if installed_path then remove_tree(installed_path) end
    end)
    if not cleaned then print("[AppStore] cleanup: " .. tostring(cleanup_err)) end
end
storage.write_file(cache .. "/result.tmp", json.encode({ ok = ok, error = not ok and tostring(err):match("^[^\n]+") or nil }))
storage.rename(cache .. "/result.tmp", cache .. "/result.json")
