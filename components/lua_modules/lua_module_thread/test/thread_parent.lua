local thread = require("thread")
local delay = require("delay")

local sync = thread.sync

local child_a_path = args.child_a_path or "/fatfs/scripts/builtin/test/thread_child_a.lua"
local child_b_path = args.child_b_path or "/fatfs/scripts/builtin/test/thread_child_b.lua"

local queue_to_a = "thread_test_to_a"
local queue_to_b = "thread_test_to_b"
local queue_to_parent = "thread_test_to_parent"
local sem_name = "thread_test_sem"
local lock_name = "thread_test_lock"

local function cleanup()
    pcall(sync.queue_delete, queue_to_a)
    pcall(sync.queue_delete, queue_to_b)
    pcall(sync.queue_delete, queue_to_parent)
    pcall(sync.sem_delete, sem_name)
    pcall(sync.lock_delete, lock_name)
end

cleanup()

assert(sync.queue_create(queue_to_a, { depth = 4, item_size = 512 }))
assert(sync.queue_create(queue_to_b, { depth = 4, item_size = 512 }))
assert(sync.queue_create(queue_to_parent, { depth = 4, item_size = 512 }))
assert(sync.sem_create(sem_name, { max = 1, initial = 0 }))
assert(sync.lock_create(lock_name))

local started_a, result_a = thread.start(child_a_path, {
    queue_to_a = queue_to_a,
    queue_to_b = queue_to_b,
    sem_name = sem_name,
    lock_name = lock_name,
}, {
    name = "thread_child_a",
    timeout_ms = 5000,
    replace = true,
})
assert(started_a, result_a)

local started_b, result_b = thread.start(child_b_path, {
    queue_to_b = queue_to_b,
    queue_to_parent = queue_to_parent,
    sem_name = sem_name,
    lock_name = lock_name,
}, {
    name = "thread_child_b",
    timeout_ms = 5000,
    replace = true,
})
assert(started_b, result_b)

assert(sync.queue_send(queue_to_a, "parent\0child_a\0ping", 5000))

local ack = assert(sync.queue_recv(queue_to_parent, 5000))
assert(ack == "child_b\0parent\0ack")

local function wait_job_done(name)
    local deadline_ms = 5000
    local waited_ms = 0

    while waited_ms < deadline_ms do
        local ok, output = thread.get(name)
        assert(ok, output)
        if output:find('"status":"done"', 1, true) or output:find("done", 1, true) then
            return output
        end
        delay.delay_ms(100)
        waited_ms = waited_ms + 100
    end
    error(name .. " did not finish")
end

local output_a = wait_job_done("thread_child_a")
local output_b = wait_job_done("thread_child_b")
assert(output_a:find("thread_child_a ok", 1, true))
assert(output_b:find("thread_child_b ok", 1, true))

assert(sync.queue_delete(queue_to_a))
assert(sync.queue_delete(queue_to_b))
assert(sync.queue_delete(queue_to_parent))
assert(sync.sem_delete(sem_name))
assert(sync.lock_delete(lock_name))

print("thread_parent ok")
