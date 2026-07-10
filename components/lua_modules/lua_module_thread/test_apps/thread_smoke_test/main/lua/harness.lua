local thread = require("thread")
local delay = require("delay")
local sync = thread.sync

local base = args.base or "/fatfs/thread_test"
local paths = {
    blocking_recv = base .. "/blocking_recv.lua",
    child_a = base .. "/child_a.lua",
    child_b = base .. "/child_b.lua",
    consumer = base .. "/consumer.lua",
    echo_child = base .. "/echo_child.lua",
    lock_holder = base .. "/lock_holder.lua",
    lock_waiter = base .. "/lock_waiter.lua",
    producer = base .. "/producer.lua",
    sync_echo = base .. "/sync_echo.lua",
}

local function contains(text, needle)
    return text and text:find(needle, 1, true) ~= nil
end

local function assert_contains(text, needle)
    assert(contains(text, needle), "missing '" .. needle .. "' in: " .. tostring(text))
end

local function cleanup_one(kind, name)
    if kind == "queue" then
        pcall(sync.queue_recv, name, 0)
        pcall(sync.queue_recv, name, 0)
        pcall(sync.queue_recv, name, 0)
        pcall(sync.queue_delete, name)
    elseif kind == "sem" then
        pcall(sync.sem_delete, name)
    elseif kind == "lock" then
        pcall(sync.unlock, name)
        pcall(sync.lock_delete, name)
    end
end

local function cleanup(prefix)
    for _, suffix in ipairs({
        "to_a", "to_b", "to_parent", "q", "q2", "result", "ready", "in", "out",
        "prod", "sem", "lock", "block", "named", "exclusive"
    }) do
        cleanup_one("queue", prefix .. "_" .. suffix)
        cleanup_one("sem", prefix .. "_" .. suffix)
        cleanup_one("lock", prefix .. "_" .. suffix)
    end
end

local function wait_job(name, wanted, timeout_ms)
    local waited = 0
    timeout_ms = timeout_ms or 5000
    while waited <= timeout_ms do
        local ok, output = thread.get(name)
        assert(ok, output)
        for _, status in ipairs(wanted) do
            if contains(output, "status=" .. status) then
                return output
            end
        end
        delay.delay_ms(100)
        waited = waited + 100
    end
    error("job did not reach expected state: " .. name)
end

local function start_ok(path, child_args, opts)
    local ok, output = thread.start(path, child_args, opts)
    assert(ok, output)
    return output
end

local cases = {}

function cases.cleanup_all()
    for _, prefix in ipairs({
        "full", "named", "exclusive", "queue", "sem", "lock", "stopq", "stoplock",
        "syncerr", "conc"
    }) do
        cleanup(prefix)
    end
    for i = 1, 16 do
        cleanup("stable" .. tostring(i))
    end
    print("cleanup_all ok")
end

function cases.api()
    assert(type(thread.run) == "function")
    assert(type(thread.start) == "function")
    assert(type(thread.list) == "function")
    assert(type(thread.get) == "function")
    assert(type(thread.stop) == "function")
    assert(type(sync.queue_create) == "function")
    assert(type(sync.queue_send) == "function")
    assert(type(sync.queue_recv) == "function")
    assert(type(sync.queue_delete) == "function")
    assert(type(sync.sem_create) == "function")
    assert(type(sync.sem_give) == "function")
    assert(type(sync.sem_take) == "function")
    assert(type(sync.sem_delete) == "function")
    assert(type(sync.lock_create) == "function")
    assert(type(sync.lock) == "function")
    assert(type(sync.unlock) == "function")
    assert(type(sync.lock_delete) == "function")
    print("api ok")
end

function cases.run_args()
    local ok, output = thread.run(paths.sync_echo, {
        text = "hello",
        count = 7,
        flag = true,
        items = { "one", "two" },
        nested = { key = "value" },
    }, {
        timeout_ms = 3000,
    })
    assert(ok, output)
    assert_contains(output, "sync_echo ok")
    assert_contains(output, "count=7")
    print("run_args ok")
end

function cases.full_flow()
    local p = "full"
    cleanup(p)
    assert(sync.queue_create(p .. "_to_a", { depth = 4, item_size = 512 }))
    assert(sync.queue_create(p .. "_to_b", { depth = 4, item_size = 512 }))
    assert(sync.queue_create(p .. "_to_parent", { depth = 4, item_size = 512 }))
    assert(sync.sem_create(p .. "_sem", { max = 1, initial = 0 }))
    assert(sync.lock_create(p .. "_lock"))

    start_ok(paths.child_a, {
        queue_to_a = p .. "_to_a",
        queue_to_b = p .. "_to_b",
        sem_name = p .. "_sem",
        lock_name = p .. "_lock",
    }, { name = "thread_child_a", timeout_ms = 5000, replace = true })

    start_ok(paths.child_b, {
        queue_to_b = p .. "_to_b",
        queue_to_parent = p .. "_to_parent",
        sem_name = p .. "_sem",
        lock_name = p .. "_lock",
    }, { name = "thread_child_b", timeout_ms = 5000, replace = true })

    assert(sync.queue_send(p .. "_to_a", "parent\0child_a\0ping", 3000))
    local ack = assert(sync.queue_recv(p .. "_to_parent", 3000))
    assert(ack == "child_b\0parent\0ack")

    assert(sync.queue_delete(p .. "_to_a"))
    assert(sync.queue_delete(p .. "_to_b"))
    assert(sync.queue_delete(p .. "_to_parent"))
    assert(sync.sem_delete(p .. "_sem"))
    assert(sync.lock_delete(p .. "_lock"))
    print("full_flow ok")
end

function cases.job_named_replace()
    local p = "named"
    cleanup(p)
    assert(sync.queue_create(p .. "_q", { depth = 1, item_size = 32 }))
    start_ok(paths.blocking_recv, {
        queue_name = p .. "_q",
        timeout_ms = 10000,
    }, { name = "thread_named_job", timeout_ms = 0, replace = true })
    delay.delay_ms(100)

    local ok, output = thread.list("all")
    assert(ok, output)
    assert_contains(output, "thread_named_job")
    ok, output = thread.get("thread_named_job")
    assert(ok, output)
    assert_contains(output, "name=thread_named_job")

    start_ok(paths.sync_echo, {
        text = "hello",
        count = 7,
        flag = true,
        items = { "one", "two" },
        nested = { key = "value" },
    }, { name = "thread_named_job", timeout_ms = 3000, replace = true })
    delay.delay_ms(200)
    assert(sync.queue_delete(p .. "_q"))
    print("job_named_replace ok")
end

function cases.job_exclusive()
    local p = "exclusive"
    cleanup(p)
    assert(sync.queue_create(p .. "_q", { depth = 1, item_size = 32 }))
    start_ok(paths.blocking_recv, {
        queue_name = p .. "_q",
        timeout_ms = 10000,
    }, { name = "thread_exclusive_a", exclusive = "thread_test_group", timeout_ms = 0, replace = true })
    delay.delay_ms(100)

    local ok, output = thread.start(paths.sync_echo, {
        text = "hello",
        count = 7,
        flag = true,
        items = { "one", "two" },
        nested = { key = "value" },
    }, { name = "thread_exclusive_b", exclusive = "thread_test_group", timeout_ms = 3000 })
    assert(ok == nil and type(output) == "string")

    start_ok(paths.sync_echo, {
        text = "hello",
        count = 7,
        flag = true,
        items = { "one", "two" },
        nested = { key = "value" },
    }, { name = "thread_exclusive_b", exclusive = "thread_test_group", timeout_ms = 3000, replace = true })
    delay.delay_ms(200)
    assert(sync.queue_delete(p .. "_q"))
    print("job_exclusive ok")
end

function cases.queue_semantics()
    local p = "queue"
    cleanup(p)
    assert(sync.queue_create(p .. "_q", { depth = 2, item_size = 64 }))
    assert(sync.queue_send(p .. "_q", "a\0b", 0))
    assert(sync.queue_send(p .. "_q", "second", 0))
    local ok, err = sync.queue_send(p .. "_q", "third", 0)
    assert(ok == false and err == "timeout")
    ok, err = sync.queue_delete(p .. "_q")
    assert(ok == nil and err == "busy")
    assert(sync.queue_recv(p .. "_q", 0) == "a\0b")
    assert(sync.queue_recv(p .. "_q", 0) == "second")
    local msg
    msg, err = sync.queue_recv(p .. "_q", 0)
    assert(msg == nil and err == "timeout")
    assert(sync.queue_delete(p .. "_q"))
    print("queue_semantics ok")
end

function cases.semaphore_counting()
    local p = "sem"
    cleanup(p)
    assert(sync.sem_create(p .. "_sem", { max = 2, initial = 1 }))
    assert(sync.sem_take(p .. "_sem", 0))
    local ok, err = sync.sem_take(p .. "_sem", 0)
    assert(ok == false and err == "timeout")
    assert(sync.sem_give(p .. "_sem"))
    assert(sync.sem_give(p .. "_sem"))
    ok, err = sync.sem_give(p .. "_sem")
    assert(ok == false and err == "full")
    assert(sync.sem_delete(p .. "_sem"))
    print("semaphore_counting ok")
end

function cases.lock_contention()
    local p = "lock"
    cleanup(p)
    assert(sync.lock_create(p .. "_lock"))
    assert(sync.queue_create(p .. "_ready", { depth = 1, item_size = 64 }))
    assert(sync.queue_create(p .. "_result", { depth = 2, item_size = 64 }))

    start_ok(paths.lock_holder, {
        lock_name = p .. "_lock",
        ready_queue = p .. "_ready",
        hold_ms = 400,
    }, { name = "lock_holder", timeout_ms = 3000, replace = true })
    assert(sync.queue_recv(p .. "_ready", 3000) == "holder_locked")
    local ok, err = sync.unlock(p .. "_lock")
    assert(ok == false and err == "not_owner")
    ok, err = sync.lock_delete(p .. "_lock")
    assert(ok == nil and err == "busy")

    start_ok(paths.lock_waiter, {
        lock_name = p .. "_lock",
        result_queue = p .. "_result",
        short_timeout_ms = 50,
        long_timeout_ms = 3000,
    }, { name = "lock_waiter", timeout_ms = 4000, replace = true })
    assert(sync.queue_recv(p .. "_result", 3000) == "waiter_timeout")
    assert(sync.queue_recv(p .. "_result", 3000) == "waiter_locked")

    assert(sync.queue_delete(p .. "_ready"))
    assert(sync.queue_delete(p .. "_result"))
    assert(sync.lock_delete(p .. "_lock"))
    print("lock_contention ok")
end

function cases.stop_queue()
    local p = "stopq"
    cleanup(p)
    assert(sync.queue_create(p .. "_q", { depth = 1, item_size = 32 }))
    start_ok(paths.blocking_recv, {
        queue_name = p .. "_q",
        timeout_ms = 5000,
    }, { name = "blocking_recv_job", timeout_ms = 0, replace = true })
    delay.delay_ms(150)
    local ok, output = thread.stop("blocking_recv_job", 3000)
    assert(ok, output)
    assert(sync.queue_delete(p .. "_q"))
    print("stop_queue ok")
end

function cases.stop_lock()
    local p = "stoplock"
    cleanup(p)
    assert(sync.lock_create(p .. "_lock"))
    assert(sync.queue_create(p .. "_result", { depth = 1, item_size = 64 }))
    assert(sync.lock(p .. "_lock", 0))
    start_ok(paths.lock_waiter, {
        lock_name = p .. "_lock",
        result_queue = p .. "_result",
        short_timeout_ms = 50,
        long_timeout_ms = 5000,
    }, { name = "lock_wait_stop_job", timeout_ms = 0, replace = true })
    assert(sync.queue_recv(p .. "_result", 3000) == "waiter_timeout")
    local ok, output = thread.stop("lock_wait_stop_job", 3000)
    assert(ok, output)
    assert(sync.unlock(p .. "_lock"))
    assert(sync.queue_delete(p .. "_result"))
    assert(sync.lock_delete(p .. "_lock"))
    print("stop_lock ok")
end

function cases.invalid_args_opts()
    local ok, err = pcall(thread.run, paths.sync_echo, "not table")
    assert(ok == false and tostring(err):find("args must be a table", 1, true))
    ok, err = pcall(thread.start, paths.sync_echo, {}, { timeout_ms = -1 })
    assert(ok == false)
    ok, err = pcall(thread.start, paths.sync_echo, {}, { replace = "bad" })
    assert(ok == false)
    print("invalid_args_opts ok")
end

function cases.sync_errors()
    local p = "syncerr"
    cleanup(p)
    local ok, err = pcall(sync.queue_create, "")
    assert(ok == false and tostring(err):find("name length", 1, true))
    ok, err = pcall(sync.queue_create, "bad/name")
    assert(ok == false and tostring(err):find("invalid character", 1, true))
    ok, err = pcall(sync.queue_create, "abcdefghijklmnopqrstuvwxyz123456789")
    assert(ok == false and tostring(err):find("name length", 1, true))

    assert(sync.queue_create(p .. "_q", { depth = 1, item_size = 16 }))
    local created
    created, err = sync.queue_create(p .. "_q", { depth = 1, item_size = 16 })
    assert(created == nil and err == "exists")
    ok, err = pcall(sync.sem_take, p .. "_q", 0)
    assert(ok == false and tostring(err):find("different type", 1, true))
    created, err = sync.queue_delete(p .. "_missing")
    assert(created == nil and err == "not_found")
    assert(sync.queue_delete(p .. "_q"))
    print("sync_errors ok")
end

function cases.stability_cycles()
    for i = 1, 16 do
        local p = "stable" .. tostring(i)
        cleanup(p)
        assert(sync.queue_create(p .. "_in", { depth = 1, item_size = 64 }))
        assert(sync.queue_create(p .. "_out", { depth = 1, item_size = 64 }))
        start_ok(paths.echo_child, {
            in_q = p .. "_in",
            out_q = p .. "_out",
            expected = "ping" .. tostring(i),
            reply = "ack" .. tostring(i),
        }, { name = "stable_job_" .. tostring(i), timeout_ms = 3000, replace = true })
        assert(sync.queue_send(p .. "_in", "ping" .. tostring(i), 3000))
        assert(sync.queue_recv(p .. "_out", 3000) == "ack" .. tostring(i))
        assert(sync.queue_delete(p .. "_in"))
        assert(sync.queue_delete(p .. "_out"))
    end
    print("stability_cycles ok")
end

function cases.concurrency()
    local p = "conc"
    cleanup(p)
    assert(sync.queue_create(p .. "_prod", { depth = 8, item_size = 64 }))
    assert(sync.queue_create(p .. "_result", { depth = 2, item_size = 64 }))
    start_ok(paths.consumer, {
        queue_name = p .. "_prod",
        result_queue = p .. "_result",
        name = "C1",
        count = 10,
    }, { name = "consumer_1", timeout_ms = 5000, replace = true })
    start_ok(paths.consumer, {
        queue_name = p .. "_prod",
        result_queue = p .. "_result",
        name = "C2",
        count = 10,
    }, { name = "consumer_2", timeout_ms = 5000, replace = true })
    start_ok(paths.producer, {
        queue_name = p .. "_prod",
        prefix = "P1",
        count = 10,
    }, { name = "producer_1", timeout_ms = 5000, replace = true })
    start_ok(paths.producer, {
        queue_name = p .. "_prod",
        prefix = "P2",
        count = 10,
    }, { name = "producer_2", timeout_ms = 5000, replace = true })

    local r1 = assert(sync.queue_recv(p .. "_result", 5000))
    local r2 = assert(sync.queue_recv(p .. "_result", 5000))
    local total = tonumber(r1:match(":(%d+)")) + tonumber(r2:match(":(%d+)"))
    assert(total == 20)
    assert(sync.queue_delete(p .. "_prod"))
    assert(sync.queue_delete(p .. "_result"))
    print("concurrency ok")
end

local case = assert(args.case, "args.case is required")
assert(cases[case], "unknown case: " .. tostring(case))
cases[case]()
print("case " .. case .. " ok")
