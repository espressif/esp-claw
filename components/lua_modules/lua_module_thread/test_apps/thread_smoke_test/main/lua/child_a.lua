local thread = require("thread")
local sync = thread.sync

local msg = assert(sync.queue_recv(args.queue_to_a, 3000))
assert(msg == "parent\0child_a\0ping")

assert(sync.lock(args.lock_name, 3000))
assert(sync.queue_send(args.queue_to_b, "child_a\0child_b\0queue handoff", 3000))
assert(sync.unlock(args.lock_name))
assert(sync.sem_give(args.sem_name))

print("thread_child_a ok")
