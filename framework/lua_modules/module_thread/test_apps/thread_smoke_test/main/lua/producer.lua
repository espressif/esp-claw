local thread = require("thread")
local sync = thread.sync

for i = 1, args.count do
    assert(sync.queue_send(args.queue_name, args.prefix .. ":" .. tostring(i), 3000))
end

print("producer ok " .. args.prefix)
