local capability = require("capability")
local json = require("json")
local pet = require("pet")
local storage = require("storage")

local a = type(args) == "table" and args or {}

local CONVERT_URL = "https://ctbt.esp-claw.com/api/convert"
local HTTP_TIMEOUT_MS = 30000
local HTTP_MAX_FILE_BYTES = 8 * 1024 * 1024
local PET_MODULE_DIR = "pet_module"

local function string_arg(key)
    local value = a[key]
    if type(value) ~= "string" or value == "" then
        error("args." .. key .. " is required")
    end
    return value
end

local function optional_string_arg(key)
    local value = a[key]
    if type(value) ~= "string" then
        return ""
    end
    return value
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

local function parse_pet_url(url)
    local id = url:match("^https://codex%-pets%.net/#/pets/([A-Za-z0-9_-]+)$")
    if not id then
        error("invalid url format, expected https://codex-pets.net/#/pets/[id]")
    end
    return id
end

local function parse_pet_id(id)
    if not id:match("^[A-Za-z0-9_-]+$") then
        error("invalid pet id, expected A-Z, a-z, 0-9, '_' or '-'")
    end
    return id
end

local function parse_status_line(out)
    local first_line = tostring(out or ""):match("^(.-)\n") or tostring(out or "")
    local status = tonumber(first_line:match("^HTTP%s+(%d+)"))
    if not status then
        error("unexpected http_request output: " .. tostring(out))
    end
    return status, first_line
end

local function convert_and_save(source_url, save_path)
    local body = json.encode({
        url = source_url,
        screenWidth = 150,
        screenHeight = 150,
        transparent = true,
    })

    local ok, out, err = capability.call("http_request", {
        url = CONVERT_URL,
        method = "POST",
        headers = {
            ["Content-Type"] = "application/json",
        },
        body = body,
        timeout_ms = HTTP_TIMEOUT_MS,
        save_path = save_path,
        max_file_bytes = HTTP_MAX_FILE_BYTES,
    }, {
        source_cap = "pet_manager",
    })

    if not ok then
        local text = tostring(err or out or "unknown error")
        pcall(storage.remove, save_path)
        if text:find("HTTP allowlist is empty", 1, true) or
                text:find("is not in allowlist", 1, true) then
            error(text .. ". Add *.esp-claw.com to the Web Console allowlist and try again.")
        end
        error(text)
    end

    local status, first_line = parse_status_line(out)
    if status < 200 or status >= 300 then
        pcall(storage.remove, save_path)
        error(string.format("convert request failed: HTTP %d", status))
    end

    if first_line:find("file truncated", 1, true) then
        pcall(storage.remove, save_path)
        error(string.format("downloaded pet bin exceeds %d bytes", HTTP_MAX_FILE_BYTES))
    end
end

local function replace_file(download_path, target_path, backup_path)
    local had_target = storage.exists(target_path)

    pcall(storage.remove, backup_path)
    if had_target then
        local ok, err = pcall(storage.rename, target_path, backup_path)
        if not ok then
            error("failed to backup existing pet bin: " .. tostring(err))
        end
    end

    local ok, err = pcall(storage.rename, download_path, target_path)
    if not ok then
        if had_target then
            pcall(storage.rename, backup_path, target_path)
        end
        error("failed to install pet bin: " .. tostring(err))
    end

    if had_target then
        pcall(storage.remove, backup_path)
    end
end

local function trim(value)
    return tostring(value or ""):match("^%s*(.-)%s*$")
end

local function current_favorite_team()
    local explicit = trim(a.favorite_team or a.profile or a.team or a.country)
    if explicit ~= "" then
        return explicit
    end

    local ok, out = capability.call("match_watch_status", {}, {
        source_cap = "pet_manager",
        max_output_bytes = 1024,
    })
    if not ok then
        return ""
    end

    local decoded_ok, decoded = pcall(json.decode, tostring(out or "{}"))
    if not decoded_ok or type(decoded) ~= "table" then
        return ""
    end
    return trim(decoded.favorite_team)
end

local function write_fallback_metadata(bin_path, id)
    local sidecar_path = bin_path:gsub("%.bin$", ".pet.json")
    local favorite_team = current_favorite_team()
    local metadata = {
        id = id,
        name = id,
        profile = {
            team = favorite_team,
        },
        country = favorite_team,
    }
    local ok, err = storage.write_file(sidecar_path, json.encode(metadata))
    if ok == false then
        error("failed to write fallback pet metadata: " .. tostring(err))
    end
    return sidecar_path
end

local function run()
    local url = optional_string_arg("url")
    local id = optional_string_arg("id")

    if url ~= "" then
        id = parse_pet_url(url)
    elseif id ~= "" then
        id = parse_pet_id(id)
        url = "https://codex-pets.net/#/pets/" .. id
    else
        string_arg("url")
    end

    local pet_dir = storage.join_path(storage.get_root_dir(), "skills", PET_MODULE_DIR)
    local bin_file = id .. ".bin"
    local bin_path = storage.join_path(pet_dir, bin_file)
    local download_path = bin_path .. ".download"
    local backup_path = bin_path .. ".bak"

    ensure_dir(pet_dir)
    pcall(storage.remove, download_path)
    convert_and_save(url, download_path)
    if not storage.exists(download_path) then
        error("conversion completed but no pet bin was saved")
    end
    replace_file(download_path, bin_path, backup_path)
    local metadata_path = write_fallback_metadata(bin_path, id)
    pet.refresh()
    pet.select(id)

    print("Downloaded and selected pet: " .. id)
    print("Saved bin: " .. bin_path)
    print("Fallback metadata: " .. metadata_path)
end

local ok, e = pcall(run)
if not ok then
    print("Pet download failed: " .. tostring(e))
    error(tostring(e), 0)
end
