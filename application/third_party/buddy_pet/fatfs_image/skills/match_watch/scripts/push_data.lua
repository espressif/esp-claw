local watch = require("match_watch")

if not args or not args.json or args.json == "" then
    error("missing normalized match data json")
end

watch.push_data_json(args.json)
print("Match watch external data pushed")
