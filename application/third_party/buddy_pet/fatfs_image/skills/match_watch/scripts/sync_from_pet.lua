local capability = require("capability")
local json = require("json")
local pet = require("pet")

local a = type(args) == "table" and args or {}

local WORLDCUP_WATCH_PATH = "/fatfs/skills/match_watch_worldcup/scripts/watch.lua"
local WORLDCUP_COMPETITION = "FIFA World Cup"
local PROVIDER_STARTING_GRACE_S = 10

local function match_watch_running()
    local ok, out = capability.call("match_watch_status", {}, {
        source_cap = "match_watch",
        max_output_bytes = 1024,
    })
    if not ok then
        return false
    end
    local decoded_ok, decoded = pcall(json.decode, tostring(out or "{}"))
    return decoded_ok and type(decoded) == "table" and decoded.running == true
end

local function match_watch_status()
    local ok, out = capability.call("match_watch_status", {}, {
        source_cap = "match_watch",
        max_output_bytes = 1024,
    })
    if not ok then
        return ""
    end
    local decoded_ok, decoded = pcall(json.decode, tostring(out or "{}"))
    if not decoded_ok or type(decoded) ~= "table" then
        return {}
    end
    return decoded
end

local function provider_status()
    local ok, out = capability.call("match_watch_provider_status", {}, {
        source_cap = "match_watch",
        max_output_bytes = 2048,
    })
    if not ok then
        return nil
    end
    local decoded_ok, decoded = pcall(json.decode, tostring(out or "{}"))
    if not decoded_ok or type(decoded) ~= "table" then
        return nil
    end
    return decoded
end

local function provider_already_syncing_team(team)
    local status = provider_status()
    local args
    local runtime_s

    if type(status) ~= "table" then
        return false
    end
    if status.provider ~= "match_watch_worldcup" or status.path ~= WORLDCUP_WATCH_PATH then
        return false
    end
    if type(status.last_error) == "string" and status.last_error ~= "" then
        return false
    end
    runtime_s = tonumber(status.runtime_s) or 0
    if status.provider_running ~= true and runtime_s > PROVIDER_STARTING_GRACE_S then
        return false
    end
    if type(status.args) ~= "string" or status.args == "" then
        return false
    end
    local decoded_ok, decoded = pcall(json.decode, status.args)
    if not decoded_ok or type(decoded) ~= "table" then
        return false
    end
    args = decoded
    return args.team == team
end

local function find_pet(list, id)
    if not id or id == "" then
        return nil
    end
    for _, item in ipairs(list or {}) do
        if item.id == id then
            return item
        end
    end
    return nil
end

local function pet_profile(item)
    return item and item.profile and item.profile ~= "" and item.profile or
        item and item.country and item.country ~= "" and item.country or ""
end

local function first_pet_with_profile(list)
    for _, item in ipairs(list or {}) do
        if pet_profile(item) ~= "" then
            return item
        end
    end
    return nil
end

local function selected_pet_entry()
    local list = pet.list()
    local current = pet.current()
    local entry = find_pet(list, current)

    if entry then
        return entry, false
    end

    entry = first_pet_with_profile(list)
    if entry then
        return entry, true
    end

    return nil, false
end

local function open_match_watch_without_provider()
    local ok, out, err = capability.call("match_watch_open", {}, {
        source_cap = "match_watch",
        max_output_bytes = 1024,
    })
    if not ok then
        error(tostring(err or out or "match_watch_open failed"))
    end
    print("Match Watch opened without pet profile")
end

local function open_match_watch_for_team(team, source)
    if a.open == false or a.fast_open == false then
        return
    end

    local ok, out, err = capability.call("match_watch_open", {
        team = team,
        competition = WORLDCUP_COMPETITION,
        favorite_source = source,
        reminders = a.reminders ~= false,
    }, {
        source_cap = "match_watch",
        max_output_bytes = 1024,
    })
    if not ok then
        error(tostring(err or out or "match_watch_open failed"))
    end
    print("Match Watch opened before provider sync")
end

local entry, selected_default = selected_pet_entry()
local status = match_watch_status()
local favorite_team = type(status.favorite_team) == "string" and status.favorite_team or ""
local favorite_team_source = type(status.favorite_team_source) == "string" and status.favorite_team_source or ""
local selected_profile = pet_profile(entry)
local profile
local profile_source = "pet"
local fallback_team = type(a.fallback_team) == "string" and a.fallback_team ~= "" and a.fallback_team or ""
if a.only_if_running == true and not match_watch_running() then
    print("Match Watch sync skipped: runtime is not running")
    return
end

local favorite_is_user = favorite_team_source == "user"
-- Compatibility for devices that ran the older sync code: a user-followed team
-- could be saved with source=pet. If it differs from the current pet profile,
-- keep treating it as the user's explicit choice.
if favorite_team_source == "pet" and favorite_team ~= "" and selected_profile ~= "" and favorite_team ~= selected_profile then
    favorite_is_user = true
end

if a.prefer_pet_profile == true and not favorite_is_user then
    profile = selected_profile ~= "" and selected_profile or favorite_team
    profile_source = selected_profile ~= "" and "pet" or favorite_team_source
else
    profile = favorite_team ~= "" and favorite_team or selected_profile
    profile_source = favorite_team ~= "" and (favorite_is_user and "user" or favorite_team_source) or "pet"
end
if profile == "" then
    profile = fallback_team
    profile_source = "default"
end
if profile == "" then
    if a.open == false then
        print("Match Watch sync skipped: selected pet has no profile")
        return
    end
    open_match_watch_without_provider()
    return
end

open_match_watch_for_team(profile, profile_source)

if provider_already_syncing_team(profile) then
    print("Match Watch provider already syncing: team=" .. tostring(profile))
    return
end

local ok, out, err = capability.call("match_watch_provider_start", {
    path = WORLDCUP_WATCH_PATH,
    args = {
        team = profile,
        open = a.open ~= false and a.fast_open == false,
        poll_ms = tonumber(a.poll_ms) or 60000,
        idle_poll_ms = tonumber(a.idle_poll_ms) or 600000,
        favorite_source = profile_source,
    },
    name = "match_watch_worldcup",
}, {
    source_cap = "match_watch",
    max_output_bytes = 2048,
})

if not ok then
    print("match_watch_provider_start failed: err=" .. tostring(err) .. " output=" ..
        tostring(out or ""))
    error(tostring(err or out or "match_watch_provider_start failed"))
end

if selected_default and a.selected_only ~= true then
    pet.select(entry.id)
end

print(string.format("Synced Match Watch from pet: pet=%s profile=%s%s",
    entry and entry.id or "(fallback)",
    profile,
    favorite_team ~= "" and " favorite_team=true" or
    selected_default and (a.selected_only == true and " local_profile=true" or " selected_profile=true") or ""))
