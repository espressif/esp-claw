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

/// One action to evaluate.
///
/// Currently this is just the action itself. Agent identity (who is acting) is
/// deliberately *not* carried here: no built-in policy keys on it, so threading
/// it through would be a dead parameter. When a policy needs the acting
/// principal, add it back as borrowed primitives (not `claw-core`'s `AgentId` /
/// `AgentKind`) so this crate stays *below* the core and the dependency stays
/// one-directional.
#[derive(Clone, Copy, Debug)]
pub struct PermissionRequest<'a> {
    /// The action being requested.
    pub action: &'a Action,
}

impl<'a> PermissionRequest<'a> {
    /// Build a request for `action`.
    pub fn new(action: &'a Action) -> Self {
        Self { action }
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
/// let request = PermissionRequest::new(&action);
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
///     policy.evaluate(&PermissionRequest::new(&safe)),
///     PermissionDecision::Allow,
/// );
/// assert!(matches!(
///     policy.evaluate(&PermissionRequest::new(&risky)),
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
///     chain.evaluate(&PermissionRequest::new(&action)),
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
        match ask {
            Some(decision) => decision,
            None => PermissionDecision::Allow,
        }
    }
}
