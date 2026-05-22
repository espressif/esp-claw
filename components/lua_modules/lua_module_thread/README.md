# Thread Lua module

`thread` provides Lua job management and named FreeRTOS-backed synchronization
objects shared by independent Lua async jobs.

## API

### `thread`

- `thread.run(path, args, opts)` runs an absolute `.lua` path
  synchronously and returns `true, output` or `nil, err`.
- `thread.start(path, args, opts)` starts an async Lua job and returns
  `true, output` or `nil, err`.
- `thread.list(status)` lists async jobs. `status` may be `all`,
  `queued`, `running`, `done`, `failed`, `timeout`, `stopped`, or nil.
- `thread.get(job_id_or_name)` returns job status, summary, and logs.
- `thread.stop(job_id_or_name, wait_ms)` requests cooperative stop.

`args` must be a table or nil. It is encoded as a JSON object and becomes the
child script's global `args` table. `opts.timeout_ms` controls runtime timeout;
async timeout `0` means run until cancelled. Async `opts` also accepts `name`,
`exclusive`, and `replace`.

### `thread.sync`

- `thread.sync.queue_create(name, opts)` creates a queue. `opts.depth` defaults
  to `8` and is limited to `1..32`; `opts.item_size` defaults to `256` and is
  limited to `1..4096`.
- `thread.sync.queue_send(name, value, timeout_ms)` sends a Lua string as raw
  bytes. The string may contain `\0` bytes and must be no larger than the
  queue's `item_size`.
- `thread.sync.queue_recv(name, timeout_ms)` returns the received raw byte
  string. It returns `nil, "timeout"` on timeout and `nil, "stopped"` when the
  Lua job is stopped.
- `thread.sync.queue_delete(name)` deletes an idle empty queue. Queues with
  waiters or pending messages return `nil, "busy"`.
- `thread.sync.sem_create(name, opts)` creates a counting semaphore.
  `opts.max` is `1..255` and `opts.initial` is `0..max`.
- `thread.sync.sem_give(name)` gives a semaphore.
- `thread.sync.sem_take(name, timeout_ms)` returns `true` on success,
  `false, "timeout"` on timeout, and `false, "stopped"` when the Lua job is
  stopped.
- `thread.sync.sem_delete(name)` deletes an idle semaphore.
- `thread.sync.lock_create(name)` creates a mutex lock.
- `thread.sync.lock(name, timeout_ms)` returns `true` on success,
  `false, "timeout"` on timeout, and `false, "stopped"` when the Lua job is
  stopped.
- `thread.sync.unlock(name)` unlocks a mutex. Only the Lua job task that
  acquired it can unlock it.
- `thread.sync.lock_delete(name)` deletes an idle unlocked mutex.

Timeouts default to `0`, which means non-blocking. Permanent blocking is not
provided in the first version.

## Example

```lua
local thread = require("thread")

local ok, output = thread.run("/fatfs/scripts/builtin/test/thread_child_a.lua", {
    mode = "sync",
}, {
    timeout_ms = 5000,
})
assert(ok, output)
print(output)

thread.sync.queue_create("ui_cmd", { depth = 8, item_size = 2048 })
thread.sync.queue_send("ui_cmd", "set_text\0hello", 1000)
local msg, err = thread.sync.queue_recv("ui_cmd", 500)
thread.sync.queue_delete("ui_cmd")
```
