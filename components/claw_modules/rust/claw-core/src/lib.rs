//! `claw_core` — runtime primitives: orchestrator shell, channels, iteration loop.
//!
//! Layer 1: [`Orchestrator`]
//! Layer 3: [`iteration_loop::IterationLoop`]

#![allow(non_camel_case_types)]

pub mod agent;
pub mod channels;
pub mod iteration_loop;
pub mod memory;
mod orchestrator;
mod orchestrator_instance;
pub mod protocol;
pub mod session;

pub use channels::{
    ChannelEgress, ChannelEgressHub, ChannelError, ChannelIngress, ChannelIngressSink,
    ChannelTransport, InboundCommand, InboundMessage, LocalChannelIngress, OutboundMessage,
    RecordingTransport, ReplyRoute,
};
pub use claw_utils::define_prefixed_id;
pub use memory::{
    agent_store, global_store, ContextAdapter, ExtractError, ExtractedItem, Extractor, History,
    LlmCompactor, LlmExtractor, LongTermMemoryAdapter, MemoryTier, NoopExtractor,
    RuleBasedTierClassifier, TierClassifier, Transcript, AGENT_ID_PREFIX, GLOBAL_ID_PREFIX,
};
pub use orchestrator::{
    ChannelsEgressOnly, ChannelsUnset, FactorySet, FactoryUnset, Orchestrator, OrchestratorBuilder,
};
pub use protocol::Command;
pub use protocol::{IdParseError, IterationId, StepId, TaskId, WorkerId};
pub use session::{
    DeliverError, SessionError, SessionId, SessionMessage, SessionOut, SessionRecord, SessionStore,
};
// Skills moved to the standalone `claw-skill` crate; re-exported here so
// `claw_core::Skill*` stays the stable surface for existing callers.
pub use claw_skill::{
    FsSkillRegistry, SkillError, SkillGroup, SkillId, SkillMetadata, SkillRegistry, SkillSet,
};
// The tool framework moved to the standalone `claw-tool` crate; re-exported here
// so `claw_core::Tool*` stays the stable surface for existing callers.
pub use claw_tool::{
    tool_invoke_err, tool_invoke_err_with_retries, AllowedTools, Tool, ToolError, ToolGate,
    ToolGroup, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput, ToolRetryCount, ToolSet,
    ToolSetError, DEFAULT_TOOL_GROUP,
};
// The permission layer authoring surface — re-exported so callers can build the
// policies that `BaseAgentBuilder::with_permission_policy` accepts without
// depending on `claw-permission` directly. The runtime grant store stays internal.
pub use claw_permission::{
    Action, AllowAll, AskAtOrAbove, PermissionDecision, PermissionPolicy, PermissionRequest,
    PolicyChain, Resource, RiskClass,
};
