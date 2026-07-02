//! Fixed tool-call executor used by the async tool path.
//!
//! This is intentionally small and private to `claw-tool`: async tool execution is
//! moved off the main agent executor, but scheduling policy still belongs to
//! [`ToolRunner`](crate::ToolRunner). The worker creates and drives each tool
//! future on its own thread, so the future itself does not need to be `Send`.

use std::collections::VecDeque;
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
use std::thread;

use claw_utils::{async_oneshot, block_on};

use crate::handler::{
    tool_invoke_err, Tool, ToolError, ToolFuture, ToolInvocation, ToolInvokeError,
};

/// One fixed worker is enough for the current serialized tool-call loop: it moves
/// potentially blocking handlers off the main agent executor without introducing
/// new intra-batch parallelism yet.
const TOOL_EXECUTOR_WORKERS: usize = 1;
const TOOL_EXECUTOR_STACK_SIZE: usize = 32 * 1024;

static TOOL_EXECUTOR: OnceLock<Result<ToolExecutor, String>> = OnceLock::new();

type ToolJob = Box<dyn FnOnce() + Send + 'static>;

struct ToolExecutor {
    shared: Arc<Shared>,
}

struct Shared {
    queue: Mutex<Queue>,
    signal: Condvar,
}

struct Queue {
    jobs: VecDeque<ToolJob>,
}

#[derive(Clone, Debug)]
struct OwnedToolInvocation {
    id: Option<String>,
    name: String,
    arguments_json: String,
}

impl OwnedToolInvocation {
    fn borrowed(&self) -> ToolInvocation<'_> {
        ToolInvocation {
            id: self.id.as_deref(),
            name: &self.name,
            arguments_json: &self.arguments_json,
        }
    }
}

impl From<&ToolInvocation<'_>> for OwnedToolInvocation {
    fn from(call: &ToolInvocation<'_>) -> Self {
        Self {
            id: call.id.map(str::to_string),
            name: call.name.to_string(),
            arguments_json: call.arguments_json.to_string(),
        }
    }
}

impl ToolExecutor {
    fn new() -> Result<Self, String> {
        let shared = Arc::new(Shared {
            queue: Mutex::new(Queue {
                jobs: VecDeque::new(),
            }),
            signal: Condvar::new(),
        });

        for index in 0..TOOL_EXECUTOR_WORKERS {
            let worker_shared = Arc::clone(&shared);
            thread::Builder::new()
                .name(format!("claw_tool_exec_{index}"))
                .stack_size(TOOL_EXECUTOR_STACK_SIZE)
                .spawn(move || worker_loop(worker_shared))
                .map_err(|error| {
                    format!("failed to spawn tool executor worker {index}: {error}")
                })?;
        }

        Ok(Self { shared })
    }

    fn submit(&self, job: ToolJob) {
        let mut queue = lock(&self.shared.queue);
        queue.jobs.push_back(job);
        drop(queue);
        self.shared.signal.notify_one();
    }
}

pub(crate) fn invoke_on_global_executor<'a>(
    tool: Tool,
    call: &ToolInvocation<'_>,
) -> ToolFuture<'a> {
    let call = OwnedToolInvocation::from(call);
    Box::pin(async move {
        let executor = match global_executor() {
            Ok(executor) => executor,
            Err(error) => return Err(executor_unavailable(error)),
        };
        let (sender, receiver) = async_oneshot();
        executor.submit(Box::new(move || {
            let result = block_on(async move {
                let invocation = call.borrowed();
                tool.invoke_inline_async(&invocation).await
            });
            let _ = sender.send(result);
        }));
        receiver
            .await
            .unwrap_or_else(|| Err(executor_unavailable("tool executor dropped the result")))
    })
}

fn global_executor() -> Result<&'static ToolExecutor, &'static str> {
    match TOOL_EXECUTOR.get_or_init(ToolExecutor::new) {
        Ok(executor) => Ok(executor),
        Err(error) => Err(error.as_str()),
    }
}

fn executor_unavailable(message: &str) -> ToolInvokeError {
    tool_invoke_err(ToolError::invoke_rejected(format!(
        "tool executor unavailable: {message}"
    )))
}

fn worker_loop(shared: Arc<Shared>) {
    loop {
        let job = {
            let mut queue = lock(&shared.queue);
            loop {
                if let Some(job) = queue.jobs.pop_front() {
                    break job;
                }
                queue = wait(&shared.signal, queue);
            }
        };
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(job));
    }
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn wait<'a, T>(condvar: &Condvar, guard: MutexGuard<'a, T>) -> MutexGuard<'a, T> {
    condvar
        .wait(guard)
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
