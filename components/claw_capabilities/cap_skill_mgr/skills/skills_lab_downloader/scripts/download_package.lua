local capability = require("capability")
local storage = require("storage")

local a = type(args) == "table" and args or {}

local HUB_HOST = "skills-lab.esp-claw.com"
local HUB_BASE = "https://" .. HUB_HOST .. "/raw"
local HTTP_TIMEOUT_MS = 20000
local HTTP_MAX_BODY_BYTES = 65535
local HTTP_MAX_FILE_BYTES = 10 * 1024 * 1024 -- Currently we do not expect to download files larger than 10MB.

local packages = {
    skill = { remote = "", directory = "skills", manifest = "SKILL.md" },
    app = { remote = "apps/", directory = "apps", manifest = "launcher.json" },
}

local function call_http(url)
    local ok, out, err = capability.call("http_request", {
        url = url,
        method = "GET",
        timeout_ms = HTTP_TIMEOUT_MS,
        max_body_bytes = HTTP_MAX_BODY_BYTES,
    }, {
        source_cap = "skills_lab_downloader",
    })

    if not ok then
        local text = tostring(err or out or "unknown error")
        if text:find("HTTP allowlist is empty", 1, true) or
                text:find("is not in allowlist", 1, true) then
            error(text .. ". Add *.esp-claw.com to the Web Console allowlist and try again.")
        end
        error(text)
    end

    return tostring(out or "")
end

local function call_http_save(url, path)
    local ok, out, err = capability.call("http_request", {
        url = url,
        method = "GET",
        timeout_ms = HTTP_TIMEOUT_MS,
        save_path = path,
        max_file_bytes = HTTP_MAX_FILE_BYTES,
    }, {
        source_cap = "skills_lab_downloader",
    })

    if not ok then
        local text = tostring(err or out or "unknown error")
        if text:find("HTTP allowlist is empty", 1, true) or
                text:find("is not in allowlist", 1, true) then
            error(text .. ". Add *.esp-claw.com to the Web Console allowlist and try again.")
        end
        error(text)
    end

    return tostring(out or "")
end

local function parse_http_output(out)
    local first_line, body = out:match("^(.-)\n(.*)$")
    if not first_line then
        first_line = out
        body = ""
    end

    local status = tonumber(first_line:match("^HTTP%s+(%d+)"))
    if not status then
        error("unexpected http_request output: " .. out)
    end

    return status, body
end

local function parse_http_status_line(out)
    local first_line = out:match("^(.-)\n") or out
    local status = tonumber(first_line:match("^HTTP%s+(%d+)"))
    if not status then
        error("unexpected http_request output: " .. out)
    end
    return status, first_line
end

local function fetch_text(url)
    local out = call_http(url)
    local status, body = parse_http_output(out)
    return status, body
end

local function ensure_dir(path)
    if storage.exists(path) then
        return
    end

    local current = ""
    if path:sub(1, 1) == "/" then
        current = "/"
    end

    for part in path:gmatch("[^/]+") do
        current = storage.join_path(current, part)
        if not storage.exists(current) then
            local ok, err = storage.mkdir(current)
            if ok == false then
                error("failed to create directory " .. current .. ": " .. tostring(err))
            end
        end
    end
end

local function validate_files(files, manifest)
    if type(files) ~= "table" or #files == 0 then
        error("args.files must be a non-empty array")
    end
    local seen = {}
    for index, path in pairs(files) do
        if type(index) ~= "number" or index % 1 ~= 0 or index < 1 or index > #files then
            error("args.files must be an array")
        end
        if type(path) ~= "string" or path == "" or path:find("[%c\\]") or path:sub(1, 1) == "/" or path:sub(-1) == "/" or path:find("//", 1, true) then
            error("invalid package file path: " .. tostring(path))
        end
        for part in path:gmatch("[^/]+") do
            if part == "." or part == ".." then
                error("invalid package file path: " .. path)
            end
        end
        if seen[path] then
            error("duplicate package file: " .. path)
        end
        seen[path] = true
    end
    for index = 1, #files do
        if files[index] == nil then
            error("args.files must be a dense array")
        end
    end
    if not seen[manifest] then
        error("missing package manifest: " .. manifest)
    end
end

local function encode_path(path)
    return (path:gsub("[^%w%-%._~/]", function(char)
        return string.format("%%%02X", char:byte())
    end))
end

local function download_and_save(url, path)
    local out = call_http_save(url, path)
    local status, first_line = parse_http_status_line(out)
    if status ~= 200 then
        pcall(storage.remove, path)
        error(string.format("failed to download %s (HTTP %d)", url, status))
    end
    if first_line:find("file truncated", 1, true) then
        pcall(storage.remove, path)
        error(string.format("failed to download %s: file exceeds %d bytes", url, HTTP_MAX_FILE_BYTES))
    end
end

local function run()
    local package = type(a.kind) == "string" and packages[a.kind]
    if not package then
        error("args.kind must be skill or app")
    end
    if type(a.id) ~= "string" or #a.id > 63 or not a.id:match("^[A-Za-z0-9_-]+$") then
        error("args.id must match ^[A-Za-z0-9_-]{1,63}$")
    end
    local base_url = HUB_BASE .. "/" .. package.remote .. a.id
    if a.action == "fetch_metadata" then
        local status, body = fetch_text(base_url .. "/_metadata.json")
        if status ~= 200 then
            error(string.format("metadata request failed for %s:%s (HTTP %d)", a.kind, a.id, status))
        end
        print(body)
        return
    end
    if a.action ~= "install" then
        error("args.action must be fetch_metadata or install")
    end

    -- Validate the whole list before writing any package files.
    validate_files(a.files, package.manifest)
    local directory = storage.join_path(storage.get_root_dir(), package.directory, a.id)
    for _, path in ipairs(a.files) do
        local target = storage.join_path(directory, path)
        ensure_dir(target:match("^(.+)/[^/]+$"))
        download_and_save(base_url .. "/" .. encode_path(path), target)
    end
    -- The agent publishes only after every file has been downloaded.
    print(string.format("Downloaded %s:%s to %s; publication required", a.kind, a.id, directory))
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    error(err)
end
