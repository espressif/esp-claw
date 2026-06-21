local capability = require("capability")
local json = require("json")

local M = {}

local COMPETITION = "FIFA World Cup"
local PROVIDER = "openligadb-worldcup"
local BASE_URL = "https://api.openligadb.de"
local HTTP_MAX_BODY_BYTES = 65535
local TEAMS_PATH = "/fatfs/skills/match_watch_worldcup/scripts/teams.lua"

local function trim(s)
    return tostring(s or ""):match("^%s*(.-)%s*$")
end

local function lower_key(s)
    return trim(s):lower():gsub("%s+", " ")
end

local teams = dofile(TEAMS_PATH)
local canonical_team = teams.canonical_team
local localized_display_team = teams.localized_display_team
local openliga_filter = teams.openliga_filter
local team_group = teams.team_group

local function url_encode(s)
    return tostring(s or ""):gsub("([^%w%-_%.~])", function(c)
        return string.format("%%%02X", string.byte(c))
    end)
end

local function parse_http_output(out)
    local first_line, body = tostring(out or ""):match("^(.-)\n(.*)$")
    first_line = first_line or tostring(out or "")
    body = body or ""
    local status = tonumber(first_line:match("^HTTP%s+(%d+)"))
    if not status then error("unexpected http_request output: " .. tostring(out)) end
    if status < 200 or status >= 300 then
        error("OpenLigaDB request failed: HTTP " .. tostring(status) .. " " .. body)
    end
    return body
end

local function strip_openliga_noise(body)
    return tostring(body or ""):gsub('("teamIconUrl"%s*:%s*)"(.-)"', '%1""')
end

local function fetch_json(url)
    local ok, out, err = capability.call("http_request", {
        url = url,
        method = "GET",
        timeout_ms = 20000,
        max_body_bytes = HTTP_MAX_BODY_BYTES,
    }, { source_cap = "match_watch_worldcup" })
    if not ok then error(tostring(err or out or "http_request failed")) end
    return json.decode(strip_openliga_noise(parse_http_output(out)))
end

local function days_from_civil(y, m, d)
    if m <= 2 then y = y - 1 end
    local era = math.floor(y / 400)
    local yoe = y - era * 400
    local mp = m + (m > 2 and -3 or 9)
    local doy = math.floor((153 * mp + 2) / 5) + d - 1
    local doe = yoe * 365 + math.floor(yoe / 4) - math.floor(yoe / 100) + doy
    return era * 146097 + doe - 719468
end

local function parse_iso_epoch(value)
    local y, mo, d, h, mi, s, sign, oh, om = tostring(value or ""):match(
        "^(%d%d%d%d)%-(%d%d)%-(%d%d)T(%d%d):(%d%d):(%d%d)([+-])(%d%d):(%d%d)$")
    if not y then
        y, mo, d, h, mi, s = tostring(value or ""):match(
            "^(%d%d%d%d)%-(%d%d)%-(%d%d)T(%d%d):(%d%d):(%d%d)Z$")
        sign, oh, om = "+", "00", "00"
    end
    if not y then
        y, mo, d, h, mi, s = tostring(value or ""):match(
            "^(%d%d%d%d)%-(%d%d)%-(%d%d)T(%d%d):(%d%d):(%d%d)$")
        sign, oh, om = "+", "00", "00"
    end
    if not y then return 0 end
    local offset = tonumber(oh) * 3600 + tonumber(om) * 60
    if sign == "-" then offset = -offset end
    return days_from_civil(tonumber(y), tonumber(mo), tonumber(d)) * 86400
        + tonumber(h) * 3600 + tonumber(mi) * 60 + tonumber(s) - offset
end

local function beijing_labels(epoch)
    if epoch == 0 then return "", "", "" end
    local t = os.date("!*t", epoch + 8 * 3600)
    local date_label = string.format("%02d.%02d", t.month, t.day)
    local time_label = string.format("%02d:%02d", t.hour, t.min)
    return date_label, time_label, date_label .. " " .. time_label
end

local function stage_from_group(group)
    local name = lower_key(group and group.groupName or "")
    if name:find("finale", 1, true) and not name:find("halbfinale", 1, true) then return "final" end
    if name:find("halbfinale", 1, true) then return "semi_final" end
    if name:find("viertelfinale", 1, true) then return "quarter_final" end
    if name:find("achtelfinale", 1, true) then return "round_of_16" end
    return "group"
end

local function group_label(group)
    local name = trim(group and group.groupName or "")
    if name == "" then return "" end

    local label = name:match("[Gg]roup%s+([A-Za-z0-9]+)") or
        name:match("[Gg]ruppe%s+([A-Za-z0-9]+)")
    if label then return label:upper() end

    if #name <= 7 then return name end
    return ""
end

local function match_group_label(stage, group, home, away)
    local label = group_label(group)
    if label ~= "" then return label end
    if stage ~= "group" then return "" end

    label = team_group(home)
    if label ~= "" then return label end
    return team_group(away)
end

local function venue_label(match)
    return trim(match.location and match.location.locationCity or "")
end

local function best_result(results)
    local best = nil
    for _, result in ipairs(results or {}) do
        if result.pointsTeam1 ~= nil and result.pointsTeam2 ~= nil and
                (best == nil or (tonumber(result.resultOrderID) or 0) > (tonumber(best.resultOrderID) or 0)) then
            best = result
        end
    end
    return best
end

local function match_score(match)
    local result = best_result(match.matchResults)
    return result and (tostring(result.pointsTeam1) .. "-" .. tostring(result.pointsTeam2)) or "0-0"
end

local function match_state(match)
    if match.matchIsFinished == true then return "full_time" end
    local kickoff_ts = parse_iso_epoch(match.matchDateTimeUTC or match.matchDateTime)
    return (kickoff_ts > 0 and os.time() >= kickoff_ts) and "live" or "upcoming"
end

local function normalize_matches(openliga_matches)
    local matches = {}
    for _, match in ipairs(openliga_matches or {}) do
        local home = canonical_team(match.team1 and match.team1.teamName or "")
        local away = canonical_team(match.team2 and match.team2.teamName or "")
        local kickoff_ts = parse_iso_epoch(match.matchDateTimeUTC or match.matchDateTime)
        local date_label, time_label, beijing_label = beijing_labels(kickoff_ts)
        local stage = stage_from_group(match.group)
        if home ~= "" and away ~= "" then
            matches[#matches + 1] = {
                match_no = tonumber(match.matchID) or (#matches + 1),
                stage = stage,
                round = tonumber(match.group and match.group.groupOrderID) or 0,
                group = match_group_label(stage, match.group, home, away),
                home = home,
                away = away,
                home_code = trim(match.team1 and match.team1.shortName or ""),
                away_code = trim(match.team2 and match.team2.shortName or ""),
                home_display = localized_display_team(home),
                away_display = localized_display_team(away),
                date_label = date_label,
                time_label = time_label,
                beijing_label = beijing_label,
                score = match_score(match),
                kickoff_ts = kickoff_ts,
                state = match_state(match),
                venue = venue_label(match),
                knockout = stage ~= "group",
            }
        end
    end
    table.sort(matches, function(a, b)
        local ar = a.state == "live" and 0 or (a.state == "upcoming" and 1 or 2)
        local br = b.state == "live" and 0 or (b.state == "upcoming" and 1 or 2)
        if ar ~= br then return ar < br end
        if (a.kickoff_ts or 0) ~= (b.kickoff_ts or 0) then
            return ar == 2 and (a.kickoff_ts or 0) > (b.kickoff_ts or 0) or
                (a.kickoff_ts or 0) < (b.kickoff_ts or 0)
        end
        return (a.match_no or 0) < (b.match_no or 0)
    end)
    return matches
end

local function team_matches_from_list(team, matches)
    local canonical = canonical_team(team)
    local filtered = {}
    for _, match in ipairs(matches or {}) do
        if match.home == canonical or match.away == canonical then
            filtered[#filtered + 1] = match
        end
    end
    return filtered
end

local function fetch_team_matches(team, league_shortcut, league_season)
    local filter = openliga_filter(team)
    local url = string.format("%s/getmatchdata/%s/%s/%s", BASE_URL,
        url_encode(league_shortcut), url_encode(league_season), url_encode(filter))
    local matches = normalize_matches(fetch_json(url))
    if #matches > 0 then
        return matches
    end

    local all_url = string.format("%s/getmatchdata/%s/%s", BASE_URL,
        url_encode(league_shortcut), url_encode(league_season))
    return team_matches_from_list(team, normalize_matches(fetch_json(all_url)))
end

local function push_matches(team, matches, open, a)
    local payload = { schema_version = 1, provider = PROVIDER, competition = COMPETITION, team = team, matches = matches }
    local ok, out, err = capability.call("match_watch_push_data", payload, { source_cap = "match_watch_worldcup" })
    if not ok then error(tostring(err or out or "match_watch_push_data failed")) end
    if open then
        local open_ok, open_out, open_err = capability.call("match_watch_open", {
            team = team,
            competition = COMPETITION,
            reminders = a.reminders ~= false,
        }, { source_cap = "match_watch_worldcup" })
        if not open_ok then error(tostring(open_err or open_out or "match_watch_open failed")) end
    end
end

local function has_live_match(matches)
    for _, match in ipairs(matches or {}) do
        if match.state == "live" then return true end
    end
    return false
end

local function resolve_team(a)
    local requested_team = trim(a.team)
    if requested_team == "" then requested_team = trim(a.team_slug) end
    if requested_team == "" then requested_team = trim(a.team_code) end
    local team = canonical_team(requested_team)
    if team == "" then
        error("missing required args.team/team_slug, for example {\"team_slug\":\"argentina\",\"open\":true}")
    end
    return team, requested_team
end

local function league_config(a)
    return tostring(a.league_shortcut or "wm26"), tostring(a.league_season or "2026")
end

function M.run_once(a)
    a = type(a) == "table" and a or {}
    local team, requested_team = resolve_team(a)
    local league_shortcut, league_season = league_config(a)

    print(string.format("World Cup refresh args: provider=%s team=%s open=%s",
        PROVIDER, requested_team ~= "" and requested_team or "(missing)", tostring(a.open)))
    local matches = fetch_team_matches(team, league_shortcut, league_season)
    if #matches == 0 then error("no OpenLigaDB World Cup matches found for " .. team) end
    push_matches(team, matches, a.open == true, a)
    print(string.format("World Cup data pushed: provider=%s team=%s matches=%d", PROVIDER, team, #matches))
end

function M.run_watch(a)
    local delay = require("delay")

    a = type(a) == "table" and a or {}
    local team, _ = resolve_team(a)
    local league_shortcut, league_season = league_config(a)

    local DEFAULT_POLL_MS = 60 * 1000
    local DEFAULT_IDLE_POLL_MS = 10 * 60 * 1000
    local DEFAULT_RETRY_MS = 60 * 1000
    local MAX_SLEEP_MS = 60000

    local function sleep_ms(ms)
        if ms < 0 then ms = 0 end
        while ms > 0 do
            local step = ms > MAX_SLEEP_MS and MAX_SLEEP_MS or ms
            delay.delay_ms(step)
            ms = ms - step
        end
    end

    local function retry_delay_ms(failures, idle_ms)
        local retry = DEFAULT_RETRY_MS
        for _ = 2, failures do
            retry = retry * 2
            if retry >= idle_ms then
                return idle_ms
            end
        end
        return retry
    end

    local poll_ms = tonumber(a.poll_ms) or DEFAULT_POLL_MS
    local idle_poll_ms = tonumber(a.idle_poll_ms) or DEFAULT_IDLE_POLL_MS
    local max_cycles = tonumber(a.max_cycles) or 0
    local opened = a.open == true
    local cycles = 0
    local failures = 0
    if poll_ms < 30000 then poll_ms = 30000 end
    if idle_poll_ms < 60000 then idle_poll_ms = 60000 end

    print(string.format("World Cup watch start: provider=%s team=%s league=%s/%s",
        PROVIDER, team, league_shortcut, league_season))
    while true do
        local ok, matches_or_err = pcall(fetch_team_matches, team, league_shortcut, league_season)
        if ok and #matches_or_err == 0 then
            ok = false
            matches_or_err = "no OpenLigaDB World Cup matches found for " .. team
        end

        if ok then
            local matches = matches_or_err
            local push_ok, push_err = pcall(push_matches, team, matches, opened, a)
            if push_ok then
                opened = false
                failures = 0
                cycles = cycles + 1
                print(string.format("World Cup watch pushed: team=%s matches=%d cycles=%d", team, #matches, cycles))
                if max_cycles > 0 and cycles >= max_cycles then break end
                sleep_ms(has_live_match(matches) and poll_ms or idle_poll_ms)
            else
                failures = failures + 1
                local retry_ms = retry_delay_ms(failures, idle_poll_ms)
                print(string.format("World Cup watch push failed: team=%s failures=%d retry_ms=%d err=%s",
                    team, failures, retry_ms, tostring(push_err)))
                sleep_ms(retry_ms)
            end
        else
            failures = failures + 1
            local retry_ms = retry_delay_ms(failures, idle_poll_ms)
            print(string.format("World Cup watch fetch failed: team=%s failures=%d retry_ms=%d err=%s",
                team, failures, retry_ms, tostring(matches_or_err)))
            sleep_ms(retry_ms)
        end
    end
    print(string.format("World Cup watch stopped: team=%s cycles=%d", team, cycles))
end

return M
