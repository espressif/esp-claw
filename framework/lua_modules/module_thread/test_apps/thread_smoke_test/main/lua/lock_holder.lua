local thread = require("thread")
local delay = require("delay")
local sync = thread.sync

assert(sync.lock(args.lock_name, 3000))
assert(sync.queue_send(args.ready_queue, "holder_locked", 3000))
delay.delay_ms(args.hold_ms or 300)
assert(sync.unlock(args.lock_name))

print("lock_holder ok")
