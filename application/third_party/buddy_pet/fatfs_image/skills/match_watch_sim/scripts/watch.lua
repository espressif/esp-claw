local capability = require("capability")
local delay = require("delay")

local a = type(args) == "table" and args or {}

local PROVIDER = "match-watch-sim"
local COMPETITION = "Sim Preview"
local SPEED = tonumber(a.speed) or 60
local TICK_MS = tonumber(a.tick_ms) or 1000
local MAX_SLEEP_MS = 1000
local PREMATCH_S = 10 * 60
local MATCH_S = 90 * 60
local MATCH_GAP_REAL_S = tonumber(a.gap_s) or 60
local POST_MATCH_HOLD_REAL_S = tonumber(a.post_match_hold_s) or 30
local KNOCKOUT_MATCH_NO = 920

local fixtures = {
    { match_no = 910, home = "Argentina", away = "France", group = "SIM", stage = "group", round = 1 },
    { match_no = 911, home = "Argentina", away = "Brazil", group = "SIM", stage = "group", round = 1 },
    { match_no = 912, home = "Argentina", away = "Germany", group = "SIM", stage = "group", round = 1 },
}

local team_display = {
    Argentina = "阿根廷",
    Brazil = "巴西",
    France = "法国",
    Germany = "德国",
}

local team_code = {
    Argentina = "ARG",
    Brazil = "BRA",
    France = "FRA",
    Germany = "GER",
}

local goal_plans = {
    {
        { minute = 12, home = true },
        { minute = 37, home = false },
        { minute = 63, home = true },
    },
    {
        { minute = 8, home = false },
        { minute = 44, home = true },
        { minute = 76, home = true },
        { minute = 88, home = false },
    },
    {
        { minute = 21, home = true },
        { minute = 55, home = false },
    },
}

local function trim(s)
    return tostring(s or ""):match("^%s*(.-)%s*$")
end

local function sleep_ms(ms)
    if ms < 0 then
        ms = 0
    end
    while ms > 0 do
        local step = ms > MAX_SLEEP_MS and MAX_SLEEP_MS or ms
        delay.delay_ms(step)
        ms = ms - step
    end
end

local function labels(epoch)
    local t = os.date("!*t", epoch + 8 * 3600)
    local date_label = "SIM"
    local time_label = string.format("%02d:%02d", t.hour, t.min)
    return date_label, time_label, date_label .. " " .. time_label
end

local function projected_kickoff_ts(kickoff, sim_now, now)
    local delta = kickoff - sim_now

    if delta > 0 then
        return now + math.max(1, math.ceil(delta / SPEED))
    end
    return now - math.max(0, math.floor((-delta) / SPEED))
end

local function sim_to_real_time(sim_t, start_real, speed)
    return start_real + (sim_t - start_real) / speed
end

local function post_match_hold_active(kickoff, sim_now, start_real, speed, hold_s)
    local sim_end = kickoff + MATCH_S

    if sim_now < sim_end or hold_s <= 0 then
        return false
    end
    return os.time() < sim_to_real_time(sim_end, start_real, speed) + hold_s
end

local function prior_post_match_hold_active(sim_now, start_real, speed, hold_s,
                                            base_kickoff, group_span_s, fixture_index)
    for j = 1, fixture_index - 1 do
        local prev_kickoff = base_kickoff + (j - 1) * group_span_s
        if post_match_hold_active(prev_kickoff, sim_now, start_real, speed, hold_s) then
            return true
        end
    end
    return false
end

local function score_at(plan, minute)
    local home_score = 0
    local away_score = 0
    local home_goal_now = false

    for _, goal in ipairs(plan) do
        if goal.minute <= minute then
            if goal.home then
                home_score = home_score + 1
                if goal.minute == minute then
                    home_goal_now = true
                end
            else
                away_score = away_score + 1
            end
        end
    end
    return home_score, away_score, home_goal_now
end

local function ensure_standing(stats, team)
    if stats[team] == nil then
        stats[team] = {
            team = team,
            points = 0,
            goals_for = 0,
            goals_against = 0,
        }
    end
    return stats[team]
end

local function record_group_result(stats, fixture, plan)
    local home_score, away_score = score_at(plan, 90)
    local home = ensure_standing(stats, fixture.home)
    local away = ensure_standing(stats, fixture.away)

    home.goals_for = home.goals_for + home_score
    home.goals_against = home.goals_against + away_score
    away.goals_for = away.goals_for + away_score
    away.goals_against = away.goals_against + home_score

    if home_score > away_score then
        home.points = home.points + 3
    elseif home_score < away_score then
        away.points = away.points + 3
    else
        home.points = home.points + 1
        away.points = away.points + 1
    end
end

local function standing_better(a, b)
    local a_gd = a.goals_for - a.goals_against
    local b_gd = b.goals_for - b.goals_against

    if a.points ~= b.points then
        return a.points > b.points
    end
    if a_gd ~= b_gd then
        return a_gd > b_gd
    end
    if a.goals_for ~= b.goals_for then
        return a.goals_for > b.goals_for
    end
    return a.team < b.team
end

local function build_knockout_fixture()
    local stats = {}
    local standings = {}

    for i, fixture in ipairs(fixtures) do
        record_group_result(stats, fixture, goal_plans[i] or {})
    end
    for _, item in pairs(stats) do
        standings[#standings + 1] = item
    end
    table.sort(standings, standing_better)

    return {
        match_no = KNOCKOUT_MATCH_NO,
        home = standings[1] and standings[1].team or fixtures[1].home,
        away = standings[2] and standings[2].team or fixtures[1].away,
        group = "KO",
        stage = "final",
        round = 2,
        knockout = true,
    }, standings
end

local function phase_at(match_s)
    if match_s < -60 * 60 then
        return "far", 0
    end
    if match_s < 0 then
        return "upcoming", 0
    end
    if match_s < 90 * 60 then
        return "live", math.floor(match_s / 60) + 1
    end
    return "full_time", 90
end

local function make_match(fixture, index, kickoff, sim_now, now, plan)
    local match_s = sim_now - kickoff
    local phase, minute = phase_at(match_s)
    local kickoff_ts = projected_kickoff_ts(kickoff, sim_now, now)
    local date_label, time_label, beijing_label = labels(kickoff_ts)
    local home_score, away_score, home_goal_now = score_at(plan or goal_plans[index] or {}, minute)
    local score = "-"
    local state = "upcoming"

    if phase == "live" then
        score = tostring(home_score) .. "-" .. tostring(away_score)
        state = home_goal_now and "goal" or (home_score < away_score and "lost" or "live")
    elseif phase == "full_time" then
        score = tostring(home_score) .. "-" .. tostring(away_score)
        state = "full_time"
    end

    return {
        match_no = fixture.match_no,
        stage = fixture.stage or "group",
        round = fixture.round or 1,
        group = fixture.group,
        home = fixture.home,
        away = fixture.away,
        home_code = team_code[fixture.home] or fixture.home,
        away_code = team_code[fixture.away] or fixture.away,
        home_display = team_display[fixture.home] or fixture.home,
        away_display = team_display[fixture.away] or fixture.away,
        date_label = date_label,
        time_label = time_label,
        beijing_label = beijing_label,
        score = score,
        kickoff_ts = kickoff_ts,
        live_minute = phase == "live" and minute or 0,
        state = state,
        venue = COMPETITION,
        knockout = fixture.knockout == true,
    }
end

local function push(matches, team, open)
    local payload = {
        schema_version = 1,
        provider = PROVIDER,
        competition = COMPETITION,
        team = team,
        matches = matches,
    }
    local ok, out, err = capability.call("match_watch_push_data", payload, {
        source_cap = "match_watch_sim",
    })
    if not ok then
        error(tostring(err or out or "match_watch_push_data failed"))
    end

    if open then
        local open_ok, open_out, open_err = capability.call("match_watch_open", {
            team = team,
            competition = COMPETITION,
            reminders = a.reminders ~= false,
        }, {
            source_cap = "match_watch_sim",
        })
        if not open_ok then
            error(tostring(open_err or open_out or "match_watch_open failed"))
        end
    end
end

if SPEED < 1 then
    SPEED = 1
end
if TICK_MS < 100 then
    TICK_MS = 100
end
if MATCH_GAP_REAL_S < 1 then
    MATCH_GAP_REAL_S = 1
end
if POST_MATCH_HOLD_REAL_S < 0 then
    POST_MATCH_HOLD_REAL_S = 0
end

local team = trim(a.team)
if team == "" then
    team = "Argentina"
end

local opened = a.open == true
local start_real = os.time()
local base_kickoff = start_real + PREMATCH_S
local match_gap_s = MATCH_GAP_REAL_S * SPEED
local group_span_s = MATCH_S + match_gap_s
local last_group_kickoff = base_kickoff + (#fixtures - 1) * group_span_s
local knockout_reveal = last_group_kickoff + MATCH_S + match_gap_s
local knockout_kickoff = knockout_reveal
local knockout_fixture, standings = build_knockout_fixture()
local knockout_goal_plan = {
    { minute = 18, home = false },
    { minute = 49, home = true },
    { minute = 83, home = true },
}
local cycles = 0
local max_cycles = tonumber(a.max_cycles) or 0
local knockout_announced = false

print(string.format(
    "Match Watch sim provider start: team=%s speed=%d tick_ms=%d gap_real_s=%d post_match_hold_s=%d",
    team, SPEED, TICK_MS, MATCH_GAP_REAL_S, POST_MATCH_HOLD_REAL_S))

while true do
    local now = os.time()
    local sim_elapsed = (now - start_real) * SPEED
    local sim_now = start_real + sim_elapsed
    local matches = {}

    for i, fixture in ipairs(fixtures) do
        local kickoff = base_kickoff + (i - 1) * group_span_s
        if not prior_post_match_hold_active(sim_now, start_real, SPEED,
                                            POST_MATCH_HOLD_REAL_S, base_kickoff, group_span_s, i) then
            matches[#matches + 1] = make_match(fixture, i, kickoff, sim_now, now, goal_plans[i])
        end
    end

    if sim_now >= knockout_reveal and
            not post_match_hold_active(last_group_kickoff, sim_now, start_real, SPEED, POST_MATCH_HOLD_REAL_S) then
        if not knockout_announced then
            print(string.format("Match Watch sim group finished: %s %dpt, %s %dpt, knockout=%s vs %s",
                standings[1] and standings[1].team or "-",
                standings[1] and standings[1].points or 0,
                standings[2] and standings[2].team or "-",
                standings[2] and standings[2].points or 0,
                knockout_fixture.home,
                knockout_fixture.away))
            knockout_announced = true
        end
        matches[#matches + 1] = make_match(knockout_fixture, #matches + 1, knockout_kickoff,
            sim_now, now, knockout_goal_plan)
    end

    push(matches, team, opened)
    opened = false
    cycles = cycles + 1

    if max_cycles > 0 and cycles >= max_cycles then
        break
    end
    if sim_now >= knockout_kickoff + MATCH_S then
        break
    end
    sleep_ms(TICK_MS)
end

print(string.format("Match Watch sim provider stopped: cycles=%d", cycles))
