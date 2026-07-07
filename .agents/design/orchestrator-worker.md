# Orchestrator worker (drive engine behind a Send handle)

Replaces the self-driving `SubmitStream` with a single owned worker. The goal is
to delete the machinery that only existed because "the caller's `poll` drives the
session": `SharedDrive`, `SubmitStream`'s custom `poll_next`, the `Mutex`-guarded
`instances`/`drives` maps, and the `InstanceSlot` checkout/reinsert RAII.

`submit` becomes what it always should have been: enqueue a message, return a
plain event `Receiver`. Something else (a worker the orchestrator owns) drives.

## Why the old design was complex

The old `Orchestrator::submit` returned a `SubmitStream` that *was* the driver:
its `poll_next` advanced a session-shared drive future (`SharedDrive =
Rc<RefCell<Option<Pin<Box<dyn Future>>>>>`) and then read its own event channel.
That single trick forced everything else:

- Multiple live streams for one session (append/interrupt/cancel each return a
  stream) had to *cooperatively* advance one drive → `SharedDrive` + the
  "who polls first drives" rule.
- Because streams poll on the same thread re-entrantly, the state they touch
  (`instances`, `drives`) was behind `Mutex`, and an instance driven across an
  `.await` was checked out via the `InstanceSlot` RAII guard.

None of this is about parallelism (the system is already single-threaded and
`!Send`). It is all bookkeeping for "the stream is the driver."

## New shape: handle + engine

```
Orchestrator (Send handle)
  ├─ Arc<SessionStore>     // session id truth source
  ├─ command_tx: Sender<Command>
  └─ WorkerHandle          // spawned via T: ClawThread, ZST static dispatch
        │  (Send command crosses the thread boundary)
   worker thread ── E::block_on (E: ClawExecutor, injected) ──▶
        Engine (!Send)                    // built INSIDE the worker
          ├─ instances: RefCell<HashMap<SessionId, OrchestratorInstance>>  // single-owner interior mut
          ├─ drives:    RefCell<HashMap<SessionId, SessionDrive>>          // single-owner interior mut
          ├─ factory, sessions (shared Arc), next_agent_id, ...
          └─ run(command_rx): EnginePoll(inflight session drives, command_rx)
                 → concurrently multiplexes every active session's drive on this
                   one thread, emits AgentEvents into each submission's own Sender
```

- The engine is `!Send` (it owns `Box<dyn Agent>` graphs), so it is **constructed
  inside the worker thread** from `Send` config; only `Send` things
  (`command_tx`, `WorkerHandle`, `Arc<SessionStore>`, and the per-submit
  `Sender`/`Receiver`, all of which `async-channel` makes `Send`) cross the
  boundary. This satisfies `ClawThread::spawn_worker`'s `F: Send` bound the same
  way `claw-cabi`'s worker used to (it now spawns no worker of its own; this is
  the only worker on device).
- The engine is the **single owner** of `instances`/`drives`. The `Mutex`es and
  the `SharedDrive` are gone; the maps become single-threaded `RefCell`s. Every
  engine method takes `&self`, and the run loop plus each in-flight per-session
  drive future all share `&Engine` (via `Rc<Engine>`), touching the `RefCell`s
  only in short, non-`await` critical sections.
- **Concurrency is real, not cosmetic.** The device (`EspIdfHttp`) and host
  (`RealHttp`) HTTP seams are genuinely async: they yield `Poll::Pending` while a
  request is in flight (ESP-IDF loops over `ESP_ERR_HTTP_EAGAIN` with
  `yield_once().await`). So while session A waits on its LLM round, the engine
  polls session B's drive. `EnginePoll` (a hand-rolled multiplexer, same shape as
  the agent `TickBatch`/the old `claw-cabi` `WorkerPoll`) polls every in-flight
  drive plus the command receiver each wakeup. Only the `BlockingHttpAdapter`
  test double resolves in one poll; it is not used on device.
- `InstanceSlot` **stays** (now borrowing `&Engine`, reinserting into the
  `RefCell`): a session being driven has its instance checked *out* of the map so
  the drive owns it across `.await` without holding a `RefCell` borrow, and it is
  reinserted on every exit path. This is what lets multiple sessions drive
  concurrently without aliasing one map borrow.

## `submit` and session validation (synchronous, handle-side)

Session existence is checked **before** entering the worker, on the handle:

```rust
pub fn submit(&self, session: SessionId, text: String, kind: DeliveryKind) -> SubmitStream {
    let (tx, rx) = async_channel::unbounded();
    if !self.sessions.contains(session) {
        let _ = tx.try_send(AgentEvent::Error { message: SessionNotFound(session).into() });
        return SubmitStream(rx); // closed channel -> stream ends after the error
    }
    let _ = self.command_tx.try_send(Command::Submit { session, text, kind, events: tx });
    SubmitStream(rx)
}
```

- `SessionStore` lives on the handle (`Send + Sync`, shared with the engine via
  `Arc`). `session_create` / `session_list` / `session_delete` and the `submit`
  existence check are all synchronous handle-side operations; only *valid*
  commands are ever sent to the worker.
- `SubmitStream` is now just a thin newtype over `async_channel::Receiver`
  (`impl Stream`). No `drive` field, no custom `poll_next`.

## Command vocabulary

```rust
enum Command {
    Submit {
        session: SessionId,
        text: String,
        kind: DeliveryKind,
        events: EventSink,                 // this submission's event Sender
        context: Option<SharedContext>,    // per-submission tool context (see below)
    },
    DeleteSession { session: SessionId },   // engine drops the live graph
    Stop,                                    // drain then exit run(); worker joins
}
```

Session create/list live entirely on the handle (`SessionStore`), so they need no
command. `DeleteSession` is a command because the engine must drop the live agent
graph for that session. `Stop` lets `Drop`/shutdown join the worker cleanly.

Interrupt/cancel are **not** separate commands: they arrive as ordinary `Submit`s
whose `DeliveryKind` the engine folds into the target session's `SessionDrive`
(setting the `SessionControl` flags). Because the async HTTP seam yields, the
engine observes such a `Submit` between a running drive's cooperative yields — it
never needs to preempt a thread blocked in a synchronous transfer.

## Capability context (per-submission)

Moving the drive multiplexer down into the engine means the layer that *has* the
per-submission context (e.g. `claw-cabi`'s `request_id`/channel/chat ids) no
longer wraps the drive. So the context is threaded through as a type-erased
[`claw_tool::SharedContext`] (`Arc<dyn Any + Send + Sync>`):

- `claw-tool` owns a poll-scoped, thread-local context (`with_context` installs
  it around each `poll`; `current_context::<T>()` downcasts it back). It lives in
  `claw-tool` because both the setter (`claw-core`'s engine) and the reader (a
  concrete tool handler) depend on `claw-tool`.
- The engine wraps **each submission's** `drive_one_submission` with
  `claw_tool::with_context(submission.context, …)`, so the right context is
  installed for exactly that turn even while other sessions are multiplexed on the
  same thread.
- `claw-cabi` builds `Arc::new(CapabilityContextData { … })`, passes it to
  `submit_with_context`, and `CapTool::invoke` reads it back with
  `claw_tool::current_context::<CapabilityContextData>()`. Host callers pass
  `None`.

## The run loop (session multiplexing)

One engine future owns everything and multiplexes by hand (same spirit as the
current `TickBatch`/`WorkerPoll`), so there is no per-session task and no shared
state:

```
run(command_rx):
  loop select:
    - command_rx.recv():
        Submit  -> fold into that session's SessionDrive (append/interrupt/cancel
                   semantics unchanged: pending/carried + SessionControl), mark active
        Delete  -> drop the session's instance + drive
        Stop    -> stop accepting, drain active drives, return
    - any active session made progress (drive one ready batch):
        emit its AgentEvents into the submission's Sender; when a submission's turn
        ends, drop its Sender so that SubmitStream ends.
```

The per-session drive logic (`drive_interruptible`, `SessionControl`,
`DeliveryKind` folding, `StartMode`) is reused as-is — the interrupt/cancel model
is unchanged. What changes is *who* pumps it (the engine loop, not a stream).

## Channels

Unbounded for now (`async-channel::unbounded`), `try_send` from the engine so it
never blocks. Bounding for on-device memory (with a drop policy that may shed
`Reasoning` but never `Output`/`Error`) is deferred until measured.

## Layering / ownership

- `claw-core`: owns both `Orchestrator` (handle) and `Engine`, plus the
  `Command` type and the `run` loop. It takes a `T: ClawThread` **and** an
  `E: ClawExecutor` at construction and `block_on`s the engine inside the worker
  via `E::block_on` — it depends only on the `ClawExecutor` *trait*, never on a
  concrete executor. The device supplies `EspIdfExecutor` (`edge-executor`
  `block_on`, from `claw-sys`); the host supplies `TokioExecutor` (a
  current-thread tokio runtime, from `claw-interface`) so async `reqwest` +
  `TokioTimer` reach tokio's reactor. The "`!Send` + externally driven, no thread"
  rule is intentionally relaxed here.
- `claw-tool`: gains the type-erased `SharedContext` + `with_context` /
  `current_context` seam described above.
- `claw-agent`: `AgentSystem` stays a thin wrapper — it holds an `Orchestrator`
  handle (now backend-erased; `F/H/Timer` survive only as a `PhantomData` marker)
  and forwards. `AgentSystem::new` gains a `thread: T` (`T: ClawThread`) value
  argument and an `E: ClawExecutor` type argument; `on_disk` supplies `StdThread`
  + `TokioExecutor`. Adds `submit_with_context`.
- `claw-cabi`: the drive now lives in the orchestrator's worker, so `claw-cabi`
  no longer spawns a worker of its own. It holds the (now `Send + Sync`)
  `AgentSystem` directly; `submit` enqueues onto the orchestrator (returning a
  request id) and stores the returned `SubmitStream` keyed by request id, and
  `receive` **drains that stream lazily** — bounded by the caller's `timeout_ms`
  via a `block_on(or(drain, EspIdfTimer::sleep))` — accumulating `Output`/`Error`
  into the flat FFI response and re-parking partial progress on timeout.
  `EspIdfTimer` is backed by the shared `esp_timer` one-shot service (a single
  system timer task dispatches all callbacks), so the timeout costs one timer
  object, never a spawned thread — the retry-backoff path benefits identically.
  Its
  capability-context thread-local is deleted; the context rides through
  `submit_with_context`. Net: **one** worker thread on device (the `claw-core`
  drive engine); the turn runs there and just buffers events until the FFI thread
  drains them. This is possible because `SubmitStream` is now `Send`.
- `CLI` / `example`: pass a `ClawThread` + `ClawExecutor` (host `StdThread` +
  `TokioExecutor`); `submit` still yields a `Stream<Item = AgentEvent>`.

## Testing

Engine logic stays testable without a worker: tests construct the `Engine`
directly and `block_on` its drive (feeding `Command`s through an in-process
channel), exactly as the old orchestrator tests `block_on`'d `submit`. No thread
is required in a unit test.

## What gets deleted

`SharedDrive`, `SubmitStream.drive` + custom `poll_next`, `SubmitStream::settled`
special-case, `SessionDrive.shared` lifecycle, the `Mutex`es around
`instances`/`drives`, `claw-cabi`'s per-submission capability-context
thread-local combinator (the drive itself moved out of `claw-cabi`), and
`claw-cabi`'s entire worker/command-queue/`ResponseStore` condvar machinery
(`worker_loop`, `WorkerPoll`, `RuntimeCommand`, the `edge-executor` dependency) —
replaced by a lazy `receive()`-time drain.

## What stays

`DeliveryKind` (append/interrupt/cancel), `SessionControl` + preemption
checkpoints, `InstanceSlot` (now `RefCell`-backed) so concurrent session drives
each own their instance across `.await`, the per-submit event channel, and the
`EventSink` threading through `Agent::tick` → `IterationLoop` (the iteration-level
events landed earlier). `claw-cabi` now drains each submission's event stream
lazily at `receive()` time instead of on a dedicated worker (see Layering).
