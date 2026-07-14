//! Resolves a baked agent manifest and assembles the corresponding [`BaseAgent`](super::BaseAgent).

mod construction;
mod create;
mod environment;
mod error;
mod layout;
mod long_term;

use std::marker::PhantomData;
use std::sync::{Arc, RwLock};

use crate::config::ClawApiManager;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::ProfileStore;
use claw_skill::FsSkillRegistry;
use claw_tool::ToolRegistry;

use self::long_term::LongTermDeps;

pub(crate) use environment::{AgentEnvironment, ProfileAccess, TranscriptTarget};
pub(crate) use error::{FsAgentCreateError, FsAgentFactoryError};

/// Shared assembly dependencies for independently-built agents.
pub(crate) struct FsAgentFactory<
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
> {
    api_manager: Arc<RwLock<ClawApiManager>>,
    tools: Arc<ToolRegistry>,
    _http: PhantomData<fn() -> Http>,
    _timer: PhantomData<fn() -> Timer>,
    transcript_dir: String,
    long_term: LongTermDeps<Filesystem>,
    profile: ProfileStore<Filesystem>,
    skills: Arc<FsSkillRegistry<Filesystem>>,
}
