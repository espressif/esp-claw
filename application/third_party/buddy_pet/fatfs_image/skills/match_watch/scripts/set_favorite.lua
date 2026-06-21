local watch = require("match_watch")

local team = nil
if args and args.team then
    team = args.team
end

local competition = nil
if args and args.competition then
    competition = args.competition
end

watch.set_favorite(team, competition)
if team and team ~= "" then
    if competition and competition ~= "" then
        print("Favorite team: " .. team .. " (" .. competition .. ")")
    else
        print("Favorite team: " .. team)
    end
else
    print("Favorite team cleared")
end
