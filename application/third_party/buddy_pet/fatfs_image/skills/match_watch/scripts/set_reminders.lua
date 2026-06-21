local watch = require("match_watch")

local enabled = true
if args and args.enabled ~= nil then
    enabled = args.enabled and true or false
end

watch.set_reminders(enabled)
if enabled then
    print("Match watch reminders enabled")
else
    print("Match watch reminders disabled")
end
