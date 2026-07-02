//! `SharedTaskPool` — a fixed set of background workers shared across the system.
//!
//! Why a pool instead of spawning per job: on ESP-IDF (FreeRTOS) creating and
//! tearing down a task per piece of work churns the heap and risks fragmentation
//! and stack-allocation failures. The C firmware policy is to allocate worker
//! tasks once and keep them. This pool mirrors that: it spawns
//! [`PoolConfig::workers`] threads at construction through an injected
//! [`ClawThread`] (the device impl gives them a PSRAM-backed stack) and they
//! live until the pool is dropped.
//!
//! Any subsystem that needs off-tick background work (conversation compaction,
//! long-term extraction, and others later) submits it here as a [`PoolJob`]. The
//! pool is intentionally dumb: FIFO, no priorities, no result channel. A job
//! that needs to report back captures its own `Arc`/channel.

use std::collections::VecDeque;
use std::future::Future;
use std::io;
use std::pin::Pin;
use std::sync::{Arc, Condvar, Mutex};

use claw_interface::{ClawThread, CoreAffinity, Priority, WorkerHandle};

use crate::block_on;

/// A unit of background work. Runs to completion on one worker thread.
pub type PoolJob = Box<dyn FnOnce() + Send + 'static>;

/// A future created and driven on a background worker.
pub type PoolFuture = Pin<Box<dyn Future<Output = ()> + 'static>>;

/// A factory for async background work. The factory crosses to the worker
/// thread; the future itself is created there, so it does not need to be `Send`.
pub type PoolAsyncJob = Box<dyn FnOnce() -> PoolFuture + Send + 'static>;

/// Default worker stack size.
///
/// Background work can reach the LLM backend (HTTP + TLS + serde_json), the
/// deepest workload these workers run, so the stack is sized for it. Matches the
/// order of magnitude of the C agent worker stacks.
const DEFAULT_STACK_SIZE: usize = 32 * 1024;

/// Configuration for a [`SharedTaskPool`].
pub struct PoolConfig {
    /// Number of persistent worker threads. One is enough for serialized work;
    /// raise it when several subsystems run heavy jobs at once.
    pub workers: usize,
    /// Per-worker stack size in bytes.
    pub stack_size: usize,
    /// Per-worker scheduling priority (ignored on host).
    pub priority: Priority,
    /// Per-worker core affinity (ignored on host).
    pub affinity: CoreAffinity,
}

impl Default for PoolConfig {
    fn default() -> Self {
        Self {
            workers: 1,
            stack_size: DEFAULT_STACK_SIZE,
            priority: Priority::Normal,
            affinity: CoreAffinity::Any,
        }
    }
}

/// State shared between the pool handle and every worker.
struct Shared {
    queue: Mutex<Queue>,
    /// Signalled when a job is enqueued or shutdown is requested.
    signal: Condvar,
}

struct Queue {
    jobs: VecDeque<PoolJob>,
    /// Set on drop; tells idle workers to exit once the queue drains.
    shutdown: bool,
}

/// A fixed pool of background workers shared across the system.
///
/// Create one at boot with [`SharedTaskPool::new`] and share it (via `Arc`)
/// across every subsystem; submit work with [`submit`](Self::submit). Workers
/// run until the pool is dropped.
///
/// # Examples
///
/// ```
/// use std::sync::{Arc, mpsc};
///
/// use claw_interface::StdThread;
/// use claw_utils::{SharedTaskPool, PoolConfig};
///
/// // The caller supplies the spawn policy: `StdThread` on the host, the
/// // PSRAM-backed `EspIdfThread` on device.
/// let pool = SharedTaskPool::new(PoolConfig::default(), StdThread)?;
///
/// // Submit a job and wait for it to run on a worker thread.
/// let (tx, rx) = mpsc::channel();
/// pool.submit(Box::new(move || {
///     tx.send(2 + 2).unwrap();
/// }));
/// assert_eq!(rx.recv().unwrap(), 4);
/// # Ok::<(), std::io::Error>(())
/// ```
pub struct SharedTaskPool {
    shared: Arc<Shared>,
    workers: Vec<WorkerHandle>,
}

impl SharedTaskPool {
    /// Spawn the worker threads using the caller-supplied [`ClawThread`] spawner.
    /// They block on an empty queue until work arrives.
    ///
    /// This crate bakes in no default spawner: the caller injects the spawn
    /// policy — the device firmware its PSRAM-backed `EspIdfThread`, host CLIs and
    /// tests `claw_interface::StdThread`. The spawner is a zero-sized type, so the
    /// `T: ClawThread` bound is statically dispatched with no allocation or vtable.
    ///
    /// # Errors
    ///
    /// Returns the OS error if a worker thread cannot be spawned; on device that
    /// means the task/stack allocation failed.
    pub fn new<T: ClawThread>(config: PoolConfig, thread: T) -> io::Result<Self> {
        let shared = Arc::new(Shared {
            queue: Mutex::new(Queue {
                jobs: VecDeque::new(),
                shutdown: false,
            }),
            signal: Condvar::new(),
        });

        let mut workers = Vec::with_capacity(config.workers);
        for index in 0..config.workers {
            let shared = Arc::clone(&shared);
            let name = format!("claw_pool_{index}");
            let handle = thread.spawn_worker(
                &name,
                config.stack_size,
                config.priority,
                config.affinity,
                move || worker_loop(&shared),
            )?;
            workers.push(handle);
        }

        Ok(Self { shared, workers })
    }

    /// Enqueue a job. It runs on the next free worker, in submission order.
    ///
    /// Best-effort: if the pool is shutting down the job is dropped unrun.
    pub fn submit(&self, job: PoolJob) {
        let mut queue = lock(&self.shared.queue);
        if queue.shutdown {
            return;
        }
        queue.jobs.push_back(job);
        drop(queue);
        self.shared.signal.notify_one();
    }

    /// Enqueue an async job. A pool worker creates and drives the future to
    /// completion, parking between wakeups.
    pub fn submit_async(&self, job: PoolAsyncJob) {
        self.submit(Box::new(move || block_on(job())));
    }
}

impl Drop for SharedTaskPool {
    fn drop(&mut self) {
        {
            let mut queue = lock(&self.shared.queue);
            queue.shutdown = true;
        }
        self.shared.signal.notify_all();
        for handle in self.workers.drain(..) {
            // A worker only exits its loop after observing shutdown, so joining
            // cannot deadlock. `WorkerHandle::join` discards a panicked worker's
            // payload (worker panics are isolated by design).
            handle.join();
        }
    }
}

/// Worker body: take jobs until shutdown drains the queue.
fn worker_loop(shared: &Shared) {
    loop {
        let job = {
            let mut queue = lock(&shared.queue);
            loop {
                if let Some(job) = queue.jobs.pop_front() {
                    break job;
                }
                if queue.shutdown {
                    return;
                }
                queue = wait(&shared.signal, queue);
            }
        };
        // Run the job with the lock released so it never blocks submitters or
        // other workers (a job can be slow — it may hit the network).
        job();
    }
}

/// Lock a mutex, recovering the guard if a previous job panicked while holding
/// it. The pool's invariants don't depend on the poisoned data being pristine,
/// so recovering is preferable to propagating a panic into unrelated workers.
fn lock<T>(mutex: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// `Condvar::wait` with the same poison-recovery policy as [`lock`].
fn wait<'a, T>(
    signal: &Condvar,
    guard: std::sync::MutexGuard<'a, T>,
) -> std::sync::MutexGuard<'a, T> {
    signal
        .wait(guard)
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
