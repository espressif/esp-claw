local thread = require("thread")
local sync = thread.sync

local ok, err = sync.lock(args.lock_name, args.short_timeout_ms or 50)
assert(ok == false and err == "timeout")
assert(sync.queue_send(args.result_queue, "waiter_timeout", 3000))
assert(sync.lock(args.lock_name, args.long_timeout_ms or 3000))
assert(sync.unlock(args.lock_name))
assert(sync.queue_send(args.result_queue, "waiter_locked", 3000))

print("lock_waiter ok")
