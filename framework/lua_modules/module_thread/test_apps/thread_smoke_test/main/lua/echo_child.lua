local thread = require("thread")
local sync = thread.sync

local msg = assert(sync.queue_recv(args.in_q, 3000))
assert(msg == args.expected)
assert(sync.queue_send(args.out_q, args.reply, 3000))
print("echo_child ok " .. args.reply)
