//! Records human decisions on `Ask` actions so a retried call resolves without
//! asking again — and so it cannot loop forever between "ask" and "retry".

use std::collections::HashMap;

/// A recorded human decision for one action signature.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Grant {
    /// The human approved; the action may proceed.
    Granted,
    /// The human declined; the reason is handed back to the model.
    Denied(String),
}

/// Remembers the outcome of `Ask` decisions, keyed by [`Action::signature`].
///
/// An `Ask` decision pauses for a human; once they decide, the runtime records it
/// here. The next time the same verb-on-resource is requested, the store answers
/// directly (proceed / refuse) instead of asking again — which both honors the
/// decision and prevents an ask/retry loop.
///
/// # Examples
///
/// ```
/// use claw_permission::{Action, Grant, GrantStore, Resource, RiskClass};
///
/// let action = Action::new("write_file", RiskClass::Moderate)
///     .with_resource(Resource::Path("/data/x".into()));
///
/// let mut grants = GrantStore::new();
/// assert_eq!(grants.lookup(&action.signature()), None); // never asked yet
///
/// grants.grant(action.signature()); // human approved
/// assert_eq!(grants.lookup(&action.signature()), Some(&Grant::Granted));
///
/// grants.forget(&action.signature()); // ask again next time
/// assert_eq!(grants.lookup(&action.signature()), None);
/// ```
///
/// [`Action::signature`]: crate::Action::signature
#[derive(Clone, Debug, Default)]
pub struct GrantStore {
    decisions: HashMap<String, Grant>,
}

impl GrantStore {
    /// An empty store (no decisions recorded yet).
    pub fn new() -> Self {
        Self::default()
    }

    /// Record approval for `signature`.
    pub fn grant(&mut self, signature: impl Into<String>) {
        self.decisions.insert(signature.into(), Grant::Granted);
    }

    /// Record a denial for `signature`, carrying the human's reason.
    pub fn deny(&mut self, signature: impl Into<String>, reason: impl Into<String>) {
        self.decisions
            .insert(signature.into(), Grant::Denied(reason.into()));
    }

    /// The recorded decision for `signature`, or `None` if it was never asked.
    pub fn lookup(&self, signature: &str) -> Option<&Grant> {
        self.decisions.get(signature)
    }

    /// Forget the decision for `signature` (e.g. to ask again next time).
    pub fn forget(&mut self, signature: &str) {
        self.decisions.remove(signature);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn grant_then_lookup_returns_granted() {
        let mut store = GrantStore::new();
        store.grant("write_file:path:/a");
        assert_eq!(store.lookup("write_file:path:/a"), Some(&Grant::Granted));
        assert_eq!(store.lookup("write_file:path:/b"), None);
    }

    #[test]
    fn deny_records_reason_and_forget_clears() {
        let mut store = GrantStore::new();
        store.deny("rm:path:/a", "too risky");
        assert_eq!(
            store.lookup("rm:path:/a"),
            Some(&Grant::Denied("too risky".into()))
        );
        store.forget("rm:path:/a");
        assert_eq!(store.lookup("rm:path:/a"), None);
    }
}
