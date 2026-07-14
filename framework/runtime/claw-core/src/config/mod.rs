//! Multi-key LLM configuration management.
//!
//! A system may hold several LLM API configs (different providers, keys, or
//! models) used for different purposes. [`ClawApiManager`] registers configs
//! against an [`ApiUsage`] and resolves the right one per usage, falling back to
//! a registered default.

pub(crate) mod catalog;
mod reasoning;

pub use reasoning::ReasoningEffort;

use std::collections::HashMap;

use claw_api::{ClawApiConfig, InitError};

/// What an LLM API config is used for.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ApiUsage {
    /// The root (externally-visible) agent's turns.
    RootAgent,
    /// Spawned subagents' turns.
    SubAgent,
    /// Long-term memory extraction / recall calls.
    Memory,
    /// Session-history compaction calls.
    Compaction,
}

/// Registers LLM API configs per [`ApiUsage`], de-duplicated by model, with a
/// default fallback.
///
/// Configs are keyed by their `model`: [`link_api`](Self::link_api) with a model
/// that already exists **replaces** the stored config (e.g. to rotate a key), and
/// every usage bound to that model then resolves to the updated config.
#[derive(Debug, Default)]
pub(crate) struct ClawApiManager {
    /// Configs by model name (one per model).
    by_model: HashMap<String, ClawApiConfig>,
    /// Usage → the model name it resolves to.
    usage: HashMap<ApiUsage, String>,
    /// Model resolved for a usage that has no explicit binding.
    default_model: Option<String>,
}

impl ClawApiManager {
    /// An empty manager. [`get_api`](Self::get_api) returns `None` for every
    /// usage until something is linked.
    #[must_use]
    pub(crate) fn new() -> Self {
        Self::default()
    }

    /// Register `api` for `usage`.
    ///
    /// If a config with the same `model` is already stored it is replaced, so
    /// every usage bound to that model sees the new config. When `default` is
    /// `true`, this model becomes the fallback for usages without an explicit
    /// binding (the most recent `default` link wins).
    ///
    /// # Errors
    ///
    /// Returns [`InitError`] without changing the manager when `api` is invalid.
    pub(crate) fn link_api(
        &mut self,
        api: ClawApiConfig,
        usage: ApiUsage,
        default: bool,
    ) -> Result<(), InitError> {
        api.validate()?;
        let model = api.model.clone();
        self.by_model.insert(model.clone(), api);
        self.usage.insert(usage, model.clone());
        if default {
            self.default_model = Some(model);
        }
        Ok(())
    }

    /// Resolve the config for `usage`: its explicit binding if present, otherwise
    /// the default, otherwise `None`.
    #[must_use]
    pub(crate) fn get_api(&self, usage: ApiUsage) -> Option<ClawApiConfig> {
        let model = self.usage.get(&usage).or(self.default_model.as_ref())?;
        self.by_model.get(model).cloned()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use claw_api::BackendKind;

    fn cfg(model: &str, key: &str) -> ClawApiConfig {
        ClawApiConfig::new(
            BackendKind::OpenAiCompatible,
            key,
            model,
            "http://api.test/v1",
        )
    }

    #[test]
    fn empty_manager_resolves_nothing() {
        let manager = ClawApiManager::new();
        assert_eq!(manager.get_api(ApiUsage::RootAgent), None);
    }

    #[test]
    fn explicit_binding_takes_precedence_over_default() {
        let mut manager = ClawApiManager::new();
        manager
            .link_api(cfg("default-model", "k0"), ApiUsage::Memory, true)
            .unwrap();
        manager
            .link_api(cfg("root-model", "k1"), ApiUsage::RootAgent, false)
            .unwrap();

        assert_eq!(
            manager.get_api(ApiUsage::RootAgent).unwrap().model,
            "root-model"
        );
        // Memory has its own binding; both it and unbound usages differ correctly.
        assert_eq!(
            manager.get_api(ApiUsage::Memory).unwrap().model,
            "default-model"
        );
    }

    #[test]
    fn unbound_usage_falls_back_to_default() {
        let mut manager = ClawApiManager::new();
        manager
            .link_api(cfg("default-model", "k0"), ApiUsage::RootAgent, true)
            .unwrap();
        // SubAgent was never linked -> falls back to the default.
        assert_eq!(
            manager.get_api(ApiUsage::SubAgent).unwrap().model,
            "default-model"
        );
    }

    #[test]
    fn unbound_usage_without_default_is_none() {
        let mut manager = ClawApiManager::new();
        manager
            .link_api(cfg("root-model", "k1"), ApiUsage::RootAgent, false)
            .unwrap();
        assert_eq!(manager.get_api(ApiUsage::Compaction), None);
    }

    #[test]
    fn invalid_config_is_rejected_without_mutating_bindings() {
        let mut manager = ClawApiManager::new();
        let invalid = cfg("invalid", "");

        assert_eq!(
            manager.link_api(invalid, ApiUsage::RootAgent, true),
            Err(InitError::MissingApiKey)
        );
        assert_eq!(manager.get_api(ApiUsage::RootAgent), None);
    }

    #[test]
    fn linking_same_model_replaces_and_updates_all_bindings() {
        let mut manager = ClawApiManager::new();
        manager
            .link_api(cfg("shared", "old-key"), ApiUsage::RootAgent, false)
            .unwrap();
        manager
            .link_api(cfg("shared", "old-key"), ApiUsage::Memory, false)
            .unwrap();
        // Re-link the same model with a rotated key.
        manager
            .link_api(cfg("shared", "new-key"), ApiUsage::RootAgent, false)
            .unwrap();

        assert_eq!(
            manager.get_api(ApiUsage::RootAgent).unwrap().api_key,
            "new-key"
        );
        // The other usage bound to the same model sees the rotated key too.
        assert_eq!(
            manager.get_api(ApiUsage::Memory).unwrap().api_key,
            "new-key"
        );
    }
}
