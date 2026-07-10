local thread = require("thread")

local sync = thread.sync

local queue_to_a = assert(args.queue_to_a, "args.queue_to_a is required")
local queue_to_b = assert(args.queue_to_b, "args.queue_to_b is required")
local sem_name = assert(args.sem_name, "args.sem_name is required")
local lock_name = assert(args.lock_name, "args.lock_name is required")

local msg = assert(sync.queue_recv(queue_to_a, 5000))
assert(msg == "parent\0child_a\0ping")

assert(sync.lock(lock_name, 5000))
assert(sync.queue_send(queue_to_b, "child_a\0child_b\0queue handoff", 5000))
assert(sync.unlock(lock_name))

assert(sync.sem_give(sem_name))

print("thread_child_a ok")
