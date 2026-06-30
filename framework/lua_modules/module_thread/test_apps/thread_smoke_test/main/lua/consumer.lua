local thread = require("thread")
local sync = thread.sync

local count = 0
for _ = 1, args.count do
    local msg = assert(sync.queue_recv(args.queue_name, 3000))
    assert(msg:find("P", 1, true) == 1)
    count = count + 1
end
assert(sync.queue_send(args.result_queue, args.name .. ":" .. tostring(count), 3000))

print("consumer ok " .. args.name .. " " .. tostring(count))
