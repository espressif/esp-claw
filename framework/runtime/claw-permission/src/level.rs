use serde::{Deserialize, Serialize};

use crate::action::RiskClass;
use crate::policy::{
    AllowAll, AskAtOrAbove, PermissionDecision, PermissionPolicy, PermissionRequest,
};

/// Session-wide handling for actions that can produce side effects.
///
/// Safe actions remain available at every level. All other actions are denied,
/// sent through human approval, or allowed according to the selected level.
#[derive(Clone, Copy, Debug, Default, Deserialize, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PermissionLevel {
    /// Refuse every action above [`RiskClass::Safe`].
    Deny,
    /// Ask for human approval for every action above [`RiskClass::Safe`].
    Ask,
    /// Run every action without asking.
    #[default]
    AllowAll,
}

impl PermissionPolicy for PermissionLevel {
    fn evaluate(&self, request: &PermissionRequest<'_>) -> PermissionDecision {
        match self {
            Self::Deny if request.action.risk() > RiskClass::Safe => PermissionDecision::Deny {
                reason: format!(
                    "'{}' is a {:?}-risk action and this session denies side effects.",
                    request.action.verb(),
                    request.action.risk()
                ),
            },
            Self::Deny | Self::AllowAll => AllowAll.evaluate(request),
            Self::Ask => AskAtOrAbove::new(RiskClass::Low).evaluate(request),
        }
    }
}
