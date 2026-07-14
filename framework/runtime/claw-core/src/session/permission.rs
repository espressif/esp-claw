use std::sync::RwLock;

use claw_permission::{PermissionDecision, PermissionLevel, PermissionPolicy, PermissionRequest};

/// Live projection of the durable session permission level.
///
/// Agents share this policy so a session command can affect their next action
/// authorization even while one of them is running outside the actor.
pub(super) struct SessionPermission {
    level: RwLock<PermissionLevel>,
}

impl SessionPermission {
    pub(super) fn new(level: PermissionLevel) -> Self {
        Self {
            level: RwLock::new(level),
        }
    }

    pub(super) fn set(&self, level: PermissionLevel) {
        *self
            .level
            .write()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = level;
    }

    fn get(&self) -> PermissionLevel {
        *self
            .level
            .read()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

impl PermissionPolicy for SessionPermission {
    fn evaluate(&self, request: &PermissionRequest<'_>) -> PermissionDecision {
        self.get().evaluate(request)
    }
}
