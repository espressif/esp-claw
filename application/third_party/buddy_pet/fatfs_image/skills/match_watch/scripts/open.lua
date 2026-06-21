local capability = require("capability")
local watch = require("match_watch")

if args and args.sync_from_pet then
    dofile("/fatfs/skills/match_watch/scripts/sync_from_pet.lua")
    return
end

if args and args.team then
    watch.set_favorite(args.team, args.competition)
elseif args and args.competition then
    watch.set_competition(args.competition)
end

if args and args.reminders ~= nil then
    watch.set_reminders(args.reminders)
end

watch.open()
print("Match watch opened")
