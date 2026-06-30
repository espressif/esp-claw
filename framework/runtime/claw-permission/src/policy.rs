//! The policy seam: turn a [`PermissionRequest`] into a [`PermissionDecision`],
//! plus the small built-in policies and the [`PolicyChain`] that composes them.

use crate::action::{Action, RiskClass};

/// The verdict a policy returns for one action.
///
/// `Ask` is the bridge to the human-approval mechanism: the runtime pauses, the
/// user decides, and a grant (or denial) is recorded so the retried call resolves
/// without asking again.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PermissionDecision {
    /// Run the tool.
    Allow,
    /// Pause and ask a human; `reason` is shown to the approver.
    Ask {
        /// Why approval is being requested (model/user-facing).
        reason: String,
    },
    /// Refuse the tool; `reason` is handed back to the model.
    Deny {
        /// Why the action was refused (model-facing).
        reason: String,
    },
}

/// One action to evaluate: who is acting (agent identity, as primitives to keep
/// this crate free of any `claw-core` dependency) and what they want to do.
///
/// `agent_id` / `agent_kind` are borrowed primitives rather than `claw-core`'s
/// `AgentId` / `AgentKind` precisely so the permission layer sits *below* the
/// core and the dependency stays one-directional.
#[derive(Clone, Copy, Debug)]
pub struct PermissionRequest<'a> {
    /// The acting agent's numeric id.
    pub agent_id: u64,
    /// The acting agent's kind (role/template name).
    pub agent_kind: &'a str,
    /// The action being requested.
    pub action: &'a Action,
}

impl<'a> PermissionRequest<'a> {
    /// Build a request for `action` by agent `agent_id` of `agent_kind`.
    pub fn new(agent_id: u64, agent_kind: &'a str, action: &'a Action) -> Self {
        Self {
            agent_id,
            agent_kind,
            action,
        }
    }
}

/// The policy interface: pure classification, no side effects.
///
/// Implement this to add a rule; compose several with [`PolicyChain`]. Object-safe
/// so a chain can hold `Box<dyn PermissionPolicy>` (heterogeneous rules), per the
/// crate's `dyn`-for-pluggable-drivers guidance.
///
/// # Examples
///
/// A custom rule that denies one verb outright and allows everything else:
///
/// ```
/// use claw_permission::{
///     Action, PermissionDecision, PermissionPolicy, PermissionRequest, RiskClass,
/// };
///
/// struct DenyVerb(&'static str);
///
/// impl PermissionPolicy for DenyVerb {
///     fn evaluate(&self, request: &PermissionRequest<'_>) -> PermissionDecision {
///         if request.action.verb() == self.0 {
///             PermissionDecision::Deny { reason: format!("'{}' is forbidden", self.0) }
///         } else {
///             PermissionDecision::Allow
///         }
///     }
/// }
///
/// let action = Action::new("rm", RiskClass::High);
/// let request = PermissionRequest::new(1, "worker", &action);
/// assert!(matches!(DenyVerb("rm").evaluate(&request), PermissionDecision::Deny { .. }));
/// ```
pub trait PermissionPolicy: Send + Sync {
    /// Classify `request` into a decision.
    fn evaluate(&self, request: &PermissionRequest<'_>) -> PermissionDecision;
}

/// The permissive default: every action is allowed. Composing a chain on top of
/// this preserves "allow unless a rule says otherwise".
#[derive(Clone, Copy, Debug, Default)]
pub struct AllowAll;

impl PermissionPolicy for AllowAll {
    fn evaluate(&self, _request: &PermissionRequest<'_>) -> PermissionDecision {
        PermissionDecision::Allow
    }
}

/// Asks for human approval when an action's risk is at or above `threshold`;
/// otherwise allows. The common "confirm risky things" rule.
///
/// # Examples
///
/// ```
/// use claw_permission::{
///     Action, AskAtOrAbove, PermissionDecision, PermissionPolicy, PermissionRequest, RiskClass,
/// };
///
/// let policy = AskAtOrAbove::new(RiskClass::Moderate);
/// let safe = Action::new("read", RiskClass::Safe);
/// let risky = Action::new("delete", RiskClass::High);
///
/// assert_eq!(
///     policy.evaluate(&PermissionRequest::new(1, "worker", &safe)),
///     PermissionDecision::Allow,
/// );
/// assert!(matches!(
///     policy.evaluate(&PermissionRequest::new(1, "worker", &risky)),
///     PermissionDecision::Ask { .. },
/// ));
/// ```
#[derive(Clone, Copy, Debug)]
pub struct AskAtOrAbove {
    threshold: RiskClass,
}

impl AskAtOrAbove {
    /// Ask at or above `threshold`.
    pub fn new(threshold: RiskClass) -> Self {
        Self { threshold }
    }
}

impl PermissionPolicy for AskAtOrAbove {
    fn evaluate(&self, request: &PermissionRequest<'_>) -> PermissionDecision {
        if request.action.risk() >= self.threshold {
            PermissionDecision::Ask {
                reason: format!(
                    "'{}' is a {:?}-risk action and needs approval.",
                    request.action.verb(),
                    request.action.risk()
                ),
            }
        } else {
            PermissionDecision::Allow
        }
    }
}

/// Composes policies, most-restrictive-wins: any `Deny` short-circuits, else any
/// `Ask` wins, else `Allow`. An empty chain allows everything.
///
/// "Most restrictive" is the safe composition: adding a rule can only ever
/// tighten access, never loosen it.
///
/// # Examples
///
/// ```
/// use claw_permission::{
///     Action, AllowAll, AskAtOrAbove, PermissionDecision, PermissionPolicy,
///     PermissionRequest, PolicyChain, RiskClass,
/// };
///
/// let chain = PolicyChain::new()
///     .with(AskAtOrAbove::new(RiskClass::Moderate))
///     .with(AllowAll);
///
/// // Ask + Allow -> Ask: the more restrictive verdict wins.
/// let action = Action::new("write", RiskClass::Moderate);
/// assert!(matches!(
///     chain.evaluate(&PermissionRequest::new(1, "worker", &action)),
///     PermissionDecision::Ask { .. },
/// ));
/// ```
#[derive(Default)]
pub struct PolicyChain {
    policies: Vec<Box<dyn PermissionPolicy>>,
}

impl PolicyChain {
    /// An empty chain (allows everything until rules are added).
    pub fn new() -> Self {
        Self::default()
    }

    /// Append a policy (builder style).
    pub fn with(mut self, policy: impl PermissionPolicy + 'static) -> Self {
        self.policies.push(Box::new(policy));
        self
    }

    /// Append a policy (mutable-reference style).
    pub fn push(&mut self, policy: impl PermissionPolicy + 'static) -> &mut Self {
        self.policies.push(Box::new(policy));
        self
    }
}

impl PermissionPolicy for PolicyChain {
    fn evaluate(&self, request: &PermissionRequest<'_>) -> PermissionDecision {
        let mut ask: Option<PermissionDecision> = None;
        for policy in &self.policies {
            match policy.evaluate(request) {
                // A single deny is final — most restrictive wins.
                deny @ PermissionDecision::Deny { .. } => return deny,
                // Remember the first ask, but keep scanning for a deny.
                decision @ PermissionDecision::Ask { .. } => ask.get_or_insert(decision),
                PermissionDecision::Allow => continue,
            };
        }
        ask.unwrap_or(PermissionDecision::Allow)
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::action::Action;

    /// A policy that always denies, for chain-composition tests.
    struct DenyAll;
    impl PermissionPolicy for DenyAll {
        fn evaluate(&self, _request: &PermissionRequest<'_>) -> PermissionDecision {
            PermissionDecision::Deny {
                reason: "nope".into(),
            }
        }
    }

    fn request_for(action: &Action) -> PermissionRequest<'_> {
        PermissionRequest::new(1, "worker", action)
    }

    #[test]
    fn allow_all_allows() {
        let action = Action::new("anything", RiskClass::High);
        assert_eq!(
            AllowAll.evaluate(&request_for(&action)),
            PermissionDecision::Allow
        );
    }

    #[test]
    fn ask_at_or_above_thresholds_on_risk() {
        let policy = AskAtOrAbove::new(RiskClass::Moderate);
        let safe = Action::new("read", RiskClass::Safe);
        let risky = Action::new("write", RiskClass::Moderate);
        assert_eq!(
            policy.evaluate(&request_for(&safe)),
            PermissionDecision::Allow
        );
        assert!(matches!(
            policy.evaluate(&request_for(&risky)),
            PermissionDecision::Ask { .. }
        ));
    }

    #[test]
    fn chain_is_most_restrictive_wins() {
        let action = Action::new("write", RiskClass::Moderate);

        // Ask + Allow -> Ask.
        let ask_chain = PolicyChain::new()
            .with(AskAtOrAbove::new(RiskClass::Moderate))
            .with(AllowAll);
        assert!(matches!(
            ask_chain.evaluate(&request_for(&action)),
            PermissionDecision::Ask { .. }
        ));

        // Deny anywhere short-circuits past an Ask.
        let deny_chain = PolicyChain::new()
            .with(AskAtOrAbove::new(RiskClass::Moderate))
            .with(DenyAll);
        assert!(matches!(
            deny_chain.evaluate(&request_for(&action)),
            PermissionDecision::Deny { .. }
        ));
    }

    #[test]
    fn empty_chain_allows() {
        let action = Action::new("x", RiskClass::High);
        assert_eq!(
            PolicyChain::new().evaluate(&request_for(&action)),
            PermissionDecision::Allow
        );
    }
}
