use crate::agent::base_agent::AgentId;
use crate::session::SessionId;

const TRANSCRIPT_DIR: &str = "transcript";
const PROFILE_DIR: &str = "profile";
const LONG_TERM_DIR: &str = "long_term";

/// Where a built agent sits in a session graph.
///
/// This is not just a transcript selector: it is the single source of truth for
/// root-only tool/profile permissions and for transcript placement and durability.
///
/// A session **root** persists its transcript, keyed by the stable [`SessionId`]
/// (so `transcript/<session id>.jsonl` is found again when the session is
/// rehydrated after a restart). A **subagent** does not persist: its transcript is
/// context-management scratch held in memory only, since its graph is rebuilt from
/// scratch on the next boot and never resumes. Because only roots write files, the
/// session-id and agent-id spaces never collide on disk.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AgentPlacement {
    /// A session's user-facing root; its transcript key is the session id.
    Root(SessionId),
    /// A spawned subagent; its transcript key is the agent id.
    Sub(AgentId),
}

impl AgentPlacement {
    pub(super) fn is_root(self) -> bool {
        matches!(self, Self::Root(_))
    }

    /// The transcript's `transcript_id`: the session id for a root, the agent id
    /// for a subagent.
    pub(super) fn transcript_id(self) -> u32 {
        match self {
            AgentPlacement::Root(session) => session.0,
            AgentPlacement::Sub(agent) => agent.0,
        }
    }

    /// Whether this placement's transcript is written to disk. Only roots persist;
    /// subagent transcripts are in-memory scratch (see the type docs).
    pub(super) fn persists(self) -> bool {
        self.is_root()
    }
}

pub(super) struct FsAgentFactoryLayout {
    pub(super) transcript_dir: String,
    pub(super) profile_dir: String,
    pub(super) long_term_dir: String,
}

impl FsAgentFactoryLayout {
    pub(super) fn new(root: String) -> Self {
        Self {
            transcript_dir: join_storage_path(&root, TRANSCRIPT_DIR),
            profile_dir: join_storage_path(&root, PROFILE_DIR),
            long_term_dir: join_storage_path(&root, LONG_TERM_DIR),
        }
    }
}

pub(super) fn join_storage_path(parent: &str, child: &str) -> String {
    if parent == "/" {
        return format!("/{child}");
    }
    let parent = parent.trim_end_matches('/');
    if parent.is_empty() {
        child.to_string()
    } else {
        format!("{parent}/{child}")
    }
}
