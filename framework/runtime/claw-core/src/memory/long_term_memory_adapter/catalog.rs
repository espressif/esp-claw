/// The adapter's rendered-catalog cache, keyed on each store's change version.
#[derive(Default)]
pub(super) struct CatalogCache {
    pub(super) global_version: u64,
    pub(super) agent_version: u64,
    pub(super) global_block: String,
    pub(super) agent_block: String,
    /// `false` until the first render populates the blocks (version 0 is a real
    /// state, an empty store, so a flag distinguishes "never rendered").
    pub(super) primed: bool,
}

/// Render a label catalog as a single durable-context line, or empty when there
/// are no labels (the context then drops the block).
pub(super) fn render_catalog(header: &str, labels: &[String]) -> String {
    if labels.is_empty() {
        String::new()
    } else {
        format!("{header}: {}", labels.join(", "))
    }
}
