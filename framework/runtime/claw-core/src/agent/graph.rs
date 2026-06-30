//! The agent-graph substrate the internal tools act on.
//!
//! This module is the *non-tool* half of multi-agent control: the back-channel an
//! agent's tools reach the orchestrator through, plus the value types that travel
//! over it. It holds no [`ToolHandler`](claw_tool::ToolHandler)s — those live in
//! [`tools`](crate::agent::tools) and call into the [`AgentContext`] façade here.
//!
//! Two seams reach the graph:
//! - **self-affecting** control (`end_conversation`) goes through the agent's own
//!   `ControlSink` (in [`tools`](crate::agent::tools)), not this module;
//! - **graph-affecting** actions (spawn / resolve-approval / delete) are emitted
//!   as a [`GraphEffect`] through a [`GraphHost`], queued and applied by the
//!   orchestrator instance *after* the tick, so a tool never mutates the live
//!   graph mid-tick. Read-only queries ([`GraphHost::snapshot`]) return
//!   synchronously.

use std::sync::Arc;

use crate::agent::base_agent::AgentId;
use crate::agent::kind::AgentKind;
use crate::agent::manifest::{AgentManifest, MANIFESTS};

/// What becomes of a subagent once it yields a result.
///
/// A subagent runs to a result and reports it to its parent regardless; this
/// policy only decides whether it then *lingers*:
/// - [`AutoOnIdle`](Self::AutoOnIdle) (default): one-shot — the subagent is
///   removed as soon as it yields, so its id immediately becomes invalid.
/// - [`Manual`](Self::Manual): persistent — after yielding (still delivering its
///   result to the parent) the subagent stays alive and idle, so the parent can
///   observe it, hand it more work, or delete it explicitly. It is removed only on
///   an explicit delete, on its parent's removal (cascade), or at session end.
///   Terminal outcomes (end / cancel / fail) always remove it, even under
///   `Manual` — there is nothing left to re-task.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TerminationPolicy {
    /// Remove the subagent as soon as it yields (one-shot).
    #[default]
    AutoOnIdle,
    /// Keep the subagent alive and idle after it yields, until explicitly removed.
    Manual,
}

impl TerminationPolicy {
    /// A short, stable, model-facing label.
    pub fn as_str(&self) -> &'static str {
        match self {
            TerminationPolicy::AutoOnIdle => "auto",
            TerminationPolicy::Manual => "manual",
        }
    }
}

/// A graph / lifecycle mutation an agent requests during its tick.
///
/// Emitted via [`GraphHost::emit`] and applied by the orchestrator instance once
/// the requesting agent's tick has returned (never mid-tick). A new internal tool
/// adds a variant here rather than a new seam, keeping [`GraphHost`] stable.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum GraphEffect {
    /// Create a child of `kind` for `goal`, parented to the emitter, with the
    /// given lifecycle policy. `id` was handed out by [`GraphHost::next_id`] so the
    /// requesting tool could report it to the model synchronously.
    Spawn {
        /// The id pre-allocated for the child.
        id: AgentId,
        /// Which kind (role/template) of agent to create.
        kind: AgentKind,
        /// Optional human/model-facing name for the child, assigned by the
        /// spawner. Purely a label for supervision (shown by `list_subagents` /
        /// `watch_subagent`); it does not identify the agent — `delete_subagent`
        /// and every other operation still key on [`AgentId`]. `None` when the
        /// spawner did not name it.
        name: Option<String>,
        /// The goal handed to the child.
        goal: String,
        /// What becomes of the child once it yields (one-shot vs. persistent).
        termination: TerminationPolicy,
    },
    /// Resolve `target`'s pending approval with the root's classified `verdict`
    /// (and optional free-text `note`).
    ResolveApproval {
        /// The agent whose pending approval is being resolved.
        target: AgentId,
        /// The root's classification of the user's reply.
        verdict: ApprovalVerdict,
        /// The user's words / reason, used when rejecting.
        note: Option<String>,
    },
    /// Remove `target` and its whole subtree. The instance honors this only when
    /// `target` is a descendant of the emitter (an agent may reap its own
    /// subagents, not arbitrary nodes).
    Delete {
        /// The subagent to remove (with everything beneath it).
        target: AgentId,
    },
}

/// A subagent's coarse lifecycle state, as observed by its parent through
/// [`AgentContext::list_subagents`] / [`AgentContext::get_subagent`].
///
/// Derived by the orchestrator instance from its own scheduling state, not asked
/// of the agent — so it reflects what the graph owner knows: queued, parked, or
/// neither.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AgentStatus {
    /// Queued to run on the next drive step.
    Ready,
    /// Parked on a human decision (an approval is outstanding).
    AwaitingApproval,
    /// Alive with nothing queued (e.g. a persistent subagent that has yielded).
    Idle,
}

impl AgentStatus {
    /// A short, stable, model-facing label.
    pub fn as_str(&self) -> &'static str {
        match self {
            AgentStatus::Ready => "ready",
            AgentStatus::AwaitingApproval => "awaiting_approval",
            AgentStatus::Idle => "idle",
        }
    }
}

/// An observable view of one live agent — what a parent sees when it lists or
/// watches its subagents. A read-only projection of the graph owner's state,
/// taken as of the start of the requesting agent's tick.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AgentSnapshot {
    /// The agent's id.
    pub id: AgentId,
    /// Its kind (role/template).
    pub kind: AgentKind,
    /// Its spawner-assigned name, or `None` if unnamed. A supervision label only
    /// — operations key on [`id`](Self::id), never this.
    pub name: Option<String>,
    /// Its creator, or `None` for a session root.
    pub parent: Option<AgentId>,
    /// Distance from the root (root = 0).
    pub depth: u16,
    /// What becomes of it once it yields (one-shot vs. persistent).
    pub termination: TerminationPolicy,
    /// Its coarse lifecycle state.
    pub status: AgentStatus,
}

/// The services an agent's graph owner exposes to that agent's internal tools.
///
/// This is the *single* back-channel from an agent to the orchestrator: identity
/// allocation ([`next_id`](Self::next_id)) plus a deferred-effect queue
/// ([`emit`](Self::emit)). The orchestrator instance implements it; tools reach
/// it through an [`AgentContext`]. Keeping the surface to these methods means new
/// tools extend [`GraphEffect`], not this trait.
pub trait GraphHost: Send + Sync {
    /// Allocate the next process-unique [`AgentId`] (e.g. for a child a tool is
    /// about to request), so the tool can report it before the child is built.
    fn next_id(&self) -> AgentId;

    /// Queue `effect`, requested by `requester`, to be applied after the current
    /// tick at a borrow-safe point. Never touches the live graph itself.
    fn emit(&self, requester: AgentId, effect: GraphEffect);

    /// A consistent read-only snapshot of every live agent, as of the start of the
    /// current tick. Reads (`list_subagents` / `watch_subagent`) return data to the
    /// model synchronously, so unlike [`emit`](Self::emit) this is not deferred.
    fn snapshot(&self) -> Vec<AgentSnapshot>;
}

/// The handle an agent's internal tools call to act on the agent graph.
///
/// One per agent, built at construction over the agent's own id and its
/// [`GraphHost`]. It is the ergonomic façade over [`GraphHost`]: tools call typed
/// methods ([`spawn`](Self::spawn), [`respond_to_approval`](Self::respond_to_approval))
/// and never touch [`GraphEffect`] or the queue directly. Self-affecting control
/// (`end_conversation`) does *not* go through here — it stays on the agent's own
/// `ControlSink`. Approval is not a tool: it is raised by the permission policy
/// (an `Ask` decision) inside the tool runner.
pub(crate) struct AgentContext {
    /// The owning agent's id — stamped as the `requester`/parent on every effect.
    id: AgentId,
    /// The graph owner this context emits effects to and draws ids from.
    host: Arc<dyn GraphHost>,
}

impl AgentContext {
    /// Build a context for agent `id` over its graph `host`.
    pub(crate) fn new(id: AgentId, host: Arc<dyn GraphHost>) -> Self {
        Self { id, host }
    }

    /// Request a child of `kind` for `goal` with lifecycle `termination` and an
    /// optional `name`; returns the id assigned to the child (allocated now, built
    /// after the tick).
    pub(crate) fn spawn(
        &self,
        kind: AgentKind,
        name: Option<String>,
        goal: String,
        termination: TerminationPolicy,
    ) -> AgentId {
        let child = self.host.next_id();
        self.host.emit(
            self.id,
            GraphEffect::Spawn {
                id: child,
                kind,
                name,
                goal,
                termination,
            },
        );
        child
    }

    /// Report the root's `verdict` (with optional `note`) for `target`'s pending
    /// approval.
    pub(crate) fn respond_to_approval(
        &self,
        target: AgentId,
        verdict: ApprovalVerdict,
        note: Option<String>,
    ) {
        self.host.emit(
            self.id,
            GraphEffect::ResolveApproval {
                target,
                verdict,
                note,
            },
        );
    }

    /// Request removal of `target` (and its subtree). The instance ignores it
    /// unless `target` is a descendant of this agent.
    pub(crate) fn delete_subagent(&self, target: AgentId) {
        self.host.emit(self.id, GraphEffect::Delete { target });
    }

    /// Every agent in this agent's subtree (its strict descendants), sorted by
    /// `(depth, id)` for a stable, parent-before-child reading order.
    pub(crate) fn list_subagents(&self) -> Vec<AgentSnapshot> {
        let all = self.host.snapshot();
        let mut descendants: Vec<AgentSnapshot> = all
            .iter()
            .filter(|snapshot| is_strict_descendant(&all, self.id, snapshot.id))
            .cloned()
            .collect();
        descendants.sort_by_key(|snapshot| (snapshot.depth, snapshot.id.0));
        descendants
    }

    /// The snapshot of `target`, but only if it is a descendant of this agent
    /// (so an agent cannot watch a sibling, an ancestor, or itself). `None`
    /// otherwise — an unknown id or one outside this agent's subtree.
    pub(crate) fn get_subagent(&self, target: AgentId) -> Option<AgentSnapshot> {
        let all = self.host.snapshot();
        is_strict_descendant(&all, self.id, target)
            .then(|| all.into_iter().find(|snapshot| snapshot.id == target))
            .flatten()
    }
}

/// The parent of `id` in a snapshot set, or `None` if absent / a root.
fn snapshot_parent(all: &[AgentSnapshot], id: AgentId) -> Option<AgentId> {
    all.iter()
        .find(|snapshot| snapshot.id == id)
        .and_then(|snapshot| snapshot.parent)
}

/// Whether `node` is a strict descendant of `ancestor` (walking parent edges in
/// the snapshot). `false` when `node == ancestor` or the chain never reaches it.
fn is_strict_descendant(all: &[AgentSnapshot], ancestor: AgentId, node: AgentId) -> bool {
    let mut current = snapshot_parent(all, node);
    while let Some(parent) = current {
        if parent == ancestor {
            return true;
        }
        current = snapshot_parent(all, parent);
    }
    false
}

/// The root's classification of a user's reply to a pending approval.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ApprovalVerdict {
    /// A clear yes — the waiting agent is approved.
    Yes,
    /// A clear no — the waiting agent is rejected (with the note as the reason).
    No,
    /// Neither a clear yes nor no — treated as a rejection carrying the user's
    /// words, so the waiting agent can reconsider.
    Other,
}

/// Which kinds an agent may spawn — the resolved, runtime form of a manifest's
/// `spawn.allowed_kinds`.
///
/// The wildcard `"*"` is normalized to [`Any`](Self::Any) at resolution time, so
/// the magic string never reaches the per-call check; every other list becomes a
/// concrete [`Only`](Self::Only) allow-set.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum SpawnPolicy {
    /// Any kind may be spawned (`allowed_kinds` contained `"*"`).
    Any,
    /// Only these specific kinds may be spawned.
    Only(Vec<AgentKind>),
}

impl SpawnPolicy {
    /// Resolve a manifest's `allowed_kinds` into a policy: a `"*"` entry anywhere
    /// means [`Any`](Self::Any); otherwise the exact set is kept.
    pub(crate) fn from_allowed_kinds(allowed_kinds: &[AgentKind]) -> Self {
        if allowed_kinds.iter().any(|kind| kind.as_str() == "*") {
            SpawnPolicy::Any
        } else {
            SpawnPolicy::Only(allowed_kinds.to_vec())
        }
    }

    /// Whether `kind` may be spawned under this policy.
    pub(crate) fn allows(&self, kind: &AgentKind) -> bool {
        match self {
            SpawnPolicy::Any => true,
            SpawnPolicy::Only(kinds) => kinds.iter().any(|allowed| allowed == kind),
        }
    }

    /// The spawnable kinds paired with their model-facing descriptions, resolved
    /// against the baked manifests. This is what the `spawn_subagent` tool shows
    /// the model so it knows *what it may spawn* up front, instead of guessing a
    /// kind and learning by rejection.
    ///
    /// [`Any`](Self::Any) expands to every baked kind; [`Only`](Self::Only) keeps
    /// the allow-set but drops any entry without a baked manifest — such a kind
    /// could never be materialized anyway, so listing it would only invite a spawn
    /// that is silently dropped at build time.
    pub(crate) fn catalog(&self) -> Vec<(AgentKind, &'static str)> {
        match self {
            SpawnPolicy::Any => MANIFESTS
                .iter()
                .map(|manifest| (manifest.kind.clone(), manifest.description))
                .collect(),
            SpawnPolicy::Only(kinds) => kinds
                .iter()
                .filter_map(|kind| {
                    AgentManifest::for_kind(kind.as_str())
                        .map(|manifest| (kind.clone(), manifest.description))
                })
                .collect(),
        }
    }

    /// A short, model-facing description of what is permitted, for the rejection
    /// message handed back when a disallowed kind is requested.
    pub(crate) fn describe(&self) -> String {
        match self {
            SpawnPolicy::Any => "any kind".to_string(),
            SpawnPolicy::Only(kinds) if kinds.is_empty() => "(none)".to_string(),
            SpawnPolicy::Only(kinds) => kinds
                .iter()
                .map(AgentKind::as_str)
                .collect::<Vec<_>>()
                .join(", "),
        }
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
pub(crate) mod test_support {
    //! Shared graph test doubles, used by this module's tests and the
    //! per-tool test modules in [`tools`](crate::agent::tools).

    use std::sync::Mutex;

    use super::*;

    /// A [`GraphHost`] that records every emitted effect, hands out ascending ids
    /// from a private counter, and returns a preset snapshot.
    #[derive(Default)]
    pub(crate) struct RecordingHost {
        next: Mutex<usize>,
        pub(crate) effects: Mutex<Vec<(AgentId, GraphEffect)>>,
        snapshot: Mutex<Vec<AgentSnapshot>>,
    }

    impl GraphHost for RecordingHost {
        fn next_id(&self) -> AgentId {
            let mut next = self.next.lock().unwrap();
            *next += 1;
            AgentId(*next)
        }
        fn emit(&self, requester: AgentId, effect: GraphEffect) {
            self.effects.lock().unwrap().push((requester, effect));
        }
        fn snapshot(&self) -> Vec<AgentSnapshot> {
            self.snapshot.lock().unwrap().clone()
        }
    }

    /// A minimal snapshot of `id` parented to `parent` for the read-path tests.
    pub(crate) fn snap(id: usize, parent: Option<usize>, depth: u16) -> AgentSnapshot {
        AgentSnapshot {
            id: AgentId(id),
            kind: AgentKind::new("worker"),
            name: None,
            parent: parent.map(AgentId),
            depth,
            termination: TerminationPolicy::Manual,
            status: AgentStatus::Idle,
        }
    }

    /// Build a host whose snapshot is `tree`.
    pub(crate) fn host_with_tree(tree: Vec<AgentSnapshot>) -> Arc<RecordingHost> {
        let host = RecordingHost::default();
        *host.snapshot.lock().unwrap() = tree;
        Arc::new(host)
    }

    /// Build an [`AgentContext`] for agent `id` over `host`.
    pub(crate) fn context_for(host: Arc<dyn GraphHost>, id: AgentId) -> Arc<AgentContext> {
        Arc::new(AgentContext::new(id, host))
    }

    /// The kinds recorded as `Spawn` effects on `host`.
    pub(crate) fn spawned_kinds(host: &RecordingHost) -> Vec<AgentKind> {
        host.effects
            .lock()
            .unwrap()
            .iter()
            .filter_map(|(_, effect)| match effect {
                GraphEffect::Spawn { kind, .. } => Some(kind.clone()),
                GraphEffect::ResolveApproval { .. } | GraphEffect::Delete { .. } => None,
            })
            .collect()
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::test_support::*;
    use super::*;

    #[test]
    fn list_subagents_returns_strict_descendants_sorted() {
        let tree = vec![
            snap(1, None, 0),
            snap(2, Some(1), 1),
            snap(3, Some(2), 2),
            snap(9, None, 0),
        ];
        let host = host_with_tree(tree);

        let ctx1 = context_for(Arc::clone(&host) as Arc<dyn GraphHost>, AgentId(1));
        let ids: Vec<usize> = ctx1.list_subagents().iter().map(|s| s.id.0).collect();
        assert_eq!(ids, vec![2, 3]);

        let ctx2 = context_for(host as Arc<dyn GraphHost>, AgentId(2));
        let ids: Vec<usize> = ctx2.list_subagents().iter().map(|s| s.id.0).collect();
        assert_eq!(ids, vec![3]);
    }

    #[test]
    fn get_subagent_authorizes_to_the_subtree() {
        let host = host_with_tree(vec![
            snap(1, None, 0),
            snap(2, Some(1), 1),
            snap(3, Some(2), 2),
            snap(9, None, 0),
        ]);
        let ctx2 = context_for(host as Arc<dyn GraphHost>, AgentId(2));
        assert!(ctx2.get_subagent(AgentId(3)).is_some(), "a descendant");
        assert!(ctx2.get_subagent(AgentId(1)).is_none(), "an ancestor");
        assert!(ctx2.get_subagent(AgentId(2)).is_none(), "itself");
        assert!(ctx2.get_subagent(AgentId(9)).is_none(), "unrelated");
    }

    #[test]
    fn spawn_policy_resolves_wildcard_to_any() {
        assert_eq!(
            SpawnPolicy::from_allowed_kinds(&[AgentKind::new("*")]),
            SpawnPolicy::Any
        );
        assert_eq!(
            SpawnPolicy::from_allowed_kinds(&[AgentKind::new("worker")]),
            SpawnPolicy::Only(vec![AgentKind::new("worker")])
        );
    }
}
