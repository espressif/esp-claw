local thread = require("thread")
local sync = thread.sync

local msg, err = sync.queue_recv(args.queue_name, args.timeout_ms or 5000)
assert(msg == nil)
assert(err == "stopped" or err == "timeout")
print("blocking_recv " .. err)
