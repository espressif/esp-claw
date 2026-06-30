local thread = require("thread")
local sync = thread.sync

assert(sync.sem_take(args.sem_name, 3000))
local msg = assert(sync.queue_recv(args.queue_to_b, 3000))
assert(msg == "child_a\0child_b\0queue handoff")

assert(sync.lock(args.lock_name, 3000))
assert(sync.queue_send(args.queue_to_parent, "child_b\0parent\0ack", 3000))
assert(sync.unlock(args.lock_name))

print("thread_child_b ok")
