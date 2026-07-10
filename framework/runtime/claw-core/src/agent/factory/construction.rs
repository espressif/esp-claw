use std::marker::PhantomData;
use std::sync::Arc;

use claw_api::{ClawApiAsync, ClawApiConfig};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::ProfileStore;
use claw_skill::{FsSkillRegistry, SkillError};
use claw_tool::ToolRegistry;

use crate::memory::{LlmExtractor, RuleBasedTierClassifier};

use super::error::FsAgentFactoryError;
use super::layout::FsAgentFactoryLayout;
use super::long_term::LongTermDeps;
use super::FsAgentFactory;

impl<
        Filesystem: ClawFs + 'static,
        Http: ClawHttp + StreamingHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    > FsAgentFactory<Filesystem, Http, Timer>
{
    /// Build a factory over an LLM `llm_config` and one persistence root.
    ///
    /// The factory owns the memory layout below `persistence_dir`: transcripts,
    /// editable profile documents, and long-term memory. `Filesystem` selects
    /// the static filesystem HAL backend used by those stores.
    ///
    /// # Errors
    ///
    /// Returns [`FsAgentFactoryError::MissingPersistenceDir`] when the
    /// persistence root is blank, or [`FsAgentFactoryError::ExtractionLlm`] if the
    /// internal extraction LLM client cannot be initialized.
    pub fn new(
        tools: Arc<ToolRegistry>,
        llm_config: ClawApiConfig,
        persistence_dir: String,
        skill_roots: Vec<String>,
    ) -> Result<Self, FsAgentFactoryError> {
        let span = tracing::info_span!("agent.factory");
        let _enter = span.enter();
        if persistence_dir.trim().is_empty() {
            tracing::error!(name: "missing_persistence_dir", reason = "empty");
            return Err(FsAgentFactoryError::MissingPersistenceDir);
        }
        let layout = FsAgentFactoryLayout::new(persistence_dir);

        let extraction_llm = match ClawApiAsync::<Http, Timer>::init_default(llm_config.clone()) {
            Ok(llm) => llm,
            Err(error) => {
                tracing::error!(name: "extraction_llm_init_failed", kind = "init");
                return Err(FsAgentFactoryError::ExtractionLlm(error));
            }
        };
        let long_term = match LongTermDeps::<Filesystem>::from_root(
            &layout.long_term_dir,
            RuleBasedTierClassifier::shared(),
            LlmExtractor::shared(extraction_llm),
        ) {
            Ok(deps) => deps,
            Err(error) => {
                tracing::error!(name: "long_term_memory_init_failed", kind = "init");
                return Err(error.into());
            }
        };

        let profile = ProfileStore::new(&layout.profile_dir);
        let skills = build_skill_registry::<Filesystem>(skill_roots)?;

        Ok(Self {
            llm_config,
            tools,
            _http: PhantomData,
            _timer: PhantomData,
            transcript_dir: layout.transcript_dir,
            long_term,
            profile,
            skills,
        })
    }
}

/// Build the shared skill catalog from the priority-ordered `skill_roots`.
///
/// A missing root is skipped so the agent still starts; a real scan failure
/// (e.g. a malformed `SKILL.md`) aborts construction.
fn build_skill_registry<F: ClawFs + 'static>(
    skill_roots: Vec<String>,
) -> Result<Arc<FsSkillRegistry<F>>, SkillError> {
    let span = tracing::info_span!("skill.catalog");
    let _enter = span.enter();
    let mut registry = FsSkillRegistry::<F>::new();
    for root in skill_roots {
        if !F::exists(root.as_str()) {
            tracing::warn!(name: "root_missing", "");
            continue;
        }
        match registry.set_root(root) {
            Ok(next) => registry = next,
            Err(error) => {
                tracing::warn!(name: "scan_failed", kind = "set_root");
                return Err(error);
            }
        }
    }
    Ok(Arc::new(registry))
}
