local thread = require("thread")

local sync = thread.sync

local queue_to_b = assert(args.queue_to_b, "args.queue_to_b is required")
local queue_to_parent = assert(args.queue_to_parent, "args.queue_to_parent is required")
local sem_name = assert(args.sem_name, "args.sem_name is required")
local lock_name = assert(args.lock_name, "args.lock_name is required")

assert(sync.sem_take(sem_name, 5000))

local msg = assert(sync.queue_recv(queue_to_b, 5000))
assert(msg == "child_a\0child_b\0queue handoff")

assert(sync.lock(lock_name, 5000))
assert(sync.queue_send(queue_to_parent, "child_b\0parent\0ack", 5000))
assert(sync.unlock(lock_name))

print("thread_child_b ok")
