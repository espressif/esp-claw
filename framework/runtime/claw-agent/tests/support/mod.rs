use std::sync::atomic::{AtomicU64, Ordering};

use claw_agent::{AgentPersistenceConfig, AgentSystem, SessionEvent, SessionEventStream};
use claw_api::{BackendKind, ClawApiConfig};
use claw_interface::{
    BlockingHttpAdapter, ImmediateTimer, MemFs, SharedScriptHttp, StdThread, TokioExecutor,
};
use futures_lite::StreamExt;
use serde_json::json;

pub type MemAgentSystem = AgentSystem<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>;

static MEM_ROOT_ID: AtomicU64 = AtomicU64::new(1);

pub fn serialize_script() -> std::sync::MutexGuard<'static, ()> {
    SharedScriptHttp::serialize()
}

pub fn mem_root(name: &str) -> String {
    let id = MEM_ROOT_ID.fetch_add(1, Ordering::Relaxed);
    format!("/{name}-{id}")
}

pub fn build_mem_system(root: &str, bodies: Vec<String>) -> MemAgentSystem {
    install_script(bodies);
    MemAgentSystem::new::<StdThread, TokioExecutor>(llm_config(), persistence(root)).unwrap()
}

pub fn assistant_text(text: &str) -> String {
    json!({ "choices": [{ "message": { "role": "assistant", "content": text } }] }).to_string()
}

pub fn drain_until_turn_ended(events: &mut SessionEventStream) -> Vec<SessionEvent> {
    futures_lite::future::block_on(async move {
        let mut collected = Vec::new();
        while let Some(event) = events.next().await {
            let ended = matches!(event, SessionEvent::TurnEnded { .. });
            collected.push(event);
            if ended {
                break;
            }
        }
        collected
    })
}

pub fn install_script(bodies: Vec<String>) {
    let mut script = Vec::with_capacity(bodies.len().saturating_add(1));
    if !bodies.is_empty() {
        script.push(assistant_text("[]"));
    }
    script.extend(bodies);
    SharedScriptHttp::install(script);
}

pub fn persistence(root: &str) -> AgentPersistenceConfig {
    AgentPersistenceConfig {
        persistence_root: root.to_string(),
        skill_roots: Vec::new(),
    }
}

pub fn llm_config() -> ClawApiConfig {
    ClawApiConfig::new(
        BackendKind::OpenAiCompatible,
        "sk-test",
        "gpt-test",
        "https://example.invalid",
    )
}
