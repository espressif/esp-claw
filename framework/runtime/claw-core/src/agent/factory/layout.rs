use crate::agent::base_agent::AgentId;
use crate::session::SessionId;

const TRANSCRIPT_DIR: &str = "sessions";
const PROFILE_DIR: &str = "profile";
const LONG_TERM_DIR: &str = "long_term";
/// Subdirectory of the sessions root holding durable session-root transcripts,
/// keyed by [`SessionId`] so they survive restarts.
const ROOT_TRANSCRIPT_DIR: &str = "roots";
/// Subdirectory of the sessions root holding ephemeral subagent transcripts,
/// keyed by [`AgentId`] (subagent graphs never resume across a restart).
const SUB_TRANSCRIPT_DIR: &str = "agents";

/// Where a built agent sits in a session graph.
///
/// This is not just a transcript selector: it is the single source of truth for
/// root-only tool/profile permissions and for transcript placement. The
/// transcript store key is deliberately decoupled from the volatile [`AgentId`]:
/// a session **root**'s record is keyed by the stable [`SessionId`] so it is found
/// again when the session is rehydrated after a restart, while a **subagent**'s
/// record is keyed by its [`AgentId`] (its graph is rebuilt from scratch on the
/// next boot and never resumes, so a fresh key is correct).
///
/// The two live under separate subdirectories so the numeric id spaces never
/// collide.
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

    /// The `(subdirectory, conversation_id)` this placement maps to under the
    /// sessions transcript root.
    pub(super) fn location(self) -> (&'static str, u32) {
        match self {
            AgentPlacement::Root(session) => (ROOT_TRANSCRIPT_DIR, session.0),
            AgentPlacement::Sub(agent) => (SUB_TRANSCRIPT_DIR, agent.0),
        }
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
