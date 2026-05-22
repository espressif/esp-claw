local capability = require("capability")
local storage = require("storage")

local a = type(args) == "table" and args or {}

local HUB_HOST = "skills-lab.esp-claw.com"
local HUB_BASE = "https://" .. HUB_HOST .. "/raw"
local HTTP_TIMEOUT_MS = 20000
local HTTP_MAX_BODY_BYTES = 65535

local function string_arg(key, default)
    local value = a[key]
    if type(value) == "string" and value ~= "" then
        return value
    end
    return default
end

local function validate_skill_name(skill_name)
    if not skill_name or skill_name == "" then
        error("args.skill_name is required")
    end
    if not skill_name:match("^[A-Za-z0-9_-]+$") then
        error("skill_name must match ^[A-Za-z0-9_-]+$")
    end
end

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

local function fetch_text(url)
    local out = call_http(url)
    local status, body = parse_http_output(out)
    return status, body
end

local function fail_for_status(skill_name, status, body)
    if status == 404 then
        error("skill not found: " .. skill_name)
    end
    error(string.format("request failed with HTTP %d: %s", status, body))
end

local function build_skill_base_dir(skill_name)
    return storage.join_path(storage.get_root_dir(), "skills", skill_name)
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

local function validate_rel_file_name(name, group_name)
    if type(name) ~= "string" or name == "" then
        error("invalid file name in extra_files." .. group_name)
    end
    if name:find("/", 1, true) or name:find("\\", 1, true) then
        error("extra_files." .. group_name .. " entries must be file names only: " .. name)
    end
end

local function get_extra_files()
    local extra_files = a.extra_files
    if extra_files == nil then
        return {}
    end
    if type(extra_files) ~= "table" then
        error("args.extra_files must be an object")
    end
    return extra_files
end

local function write_text_file(path, content)
    local ok, err = storage.write_file(path, content)
    if ok == false then
        error("failed to write file " .. path .. ": " .. tostring(err))
    end
end

local function download_and_save(url, path)
    local status, body = fetch_text(url)
    if status ~= 200 then
        error(string.format("failed to download %s (HTTP %d)", url, status))
    end
    write_text_file(path, body)
end

local function fetch_metadata(skill_name)
    local metadata_url = string.format("%s/%s/_metadata.json", HUB_BASE, skill_name)
    local status, body = fetch_text(metadata_url)
    if status ~= 200 then
        fail_for_status(skill_name, status, body)
    end
    print(body)
end

local function install_skill(skill_name)
    local skill_name_from_metadata = string_arg("skill_name_from_metadata", nil)
    if skill_name_from_metadata and skill_name_from_metadata ~= skill_name then
        error(string.format(
            "skill_name_from_metadata mismatch: expected %s, got %s",
            skill_name,
            skill_name_from_metadata))
    end

    local skill_dir = build_skill_base_dir(skill_name)
    local skill_md_url = string.format("%s/%s/SKILL.md", HUB_BASE, skill_name)
    local skill_md_path = storage.join_path(skill_dir, "SKILL.md")

    ensure_dir(skill_dir)
    download_and_save(skill_md_url, skill_md_path)

    local extra_files = get_extra_files()
    for group_name, files in pairs(extra_files) do
        if type(files) ~= "table" then
            error("extra_files." .. tostring(group_name) .. " must be an array")
        end

        local group_dir = storage.join_path(skill_dir, group_name)
        if #files > 0 then
            ensure_dir(group_dir)
        end

        for _, file_name in ipairs(files) do
            validate_rel_file_name(file_name, group_name)
            local file_url = string.format("%s/%s/%s/%s", HUB_BASE, skill_name, group_name, file_name)
            local file_path = storage.join_path(group_dir, file_name)
            download_and_save(file_url, file_path)
        end
    end

    print("Installed skill: " .. skill_name)
    print("Path: " .. skill_dir)
end

local function run()
    local action = string_arg("action", "fetch_metadata")
    local skill_name = string_arg("skill_name", nil)

    validate_skill_name(skill_name)

    if action == "fetch_metadata" then
        fetch_metadata(skill_name)
        return
    end

    if action == "install" then
        install_skill(skill_name)
        return
    end

    error("unsupported action: " .. tostring(action))
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    error(err)
end
