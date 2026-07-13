//! The block taxonomy: the fixed vocabulary of context blocks and the placement
//! metadata the builder sorts by.
//!
//! See `docs/context-model.md` for the authoritative model. This module encodes
//! only *placement* (band + scope + in-band order); block *content* is injected
//! by callers and never authored here.

use std::borrow::Cow;

/// Mutability band — the primary wire-order key. Lower bands render first and
/// form the cacheable prefix.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Band {
    /// Immutable instructions — the long shared prefix, never busted at runtime.
    Static,
    /// Slowly-mutable durable state — an edit busts only this band and below.
    Durable,
    /// Volatile tail — rebuilt each iteration, append-only between compactions.
    Volatile,
}

impl Band {
    /// Sort rank (lower renders first). Explicit to avoid `as` casts.
    pub(crate) fn rank(self) -> u8 {
        match self {
            Band::Static => 0,
            Band::Durable => 1,
            Band::Volatile => 2,
        }
    }
}

/// Ownership scope — the secondary wire-order key (broad → narrow) and the
/// reuse-sharing boundary.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Scope {
    Global,
    Session,
    Agent,
    Conversation,
    Turn,
}

impl Scope {
    /// Sort rank within a band (broad → narrow). Explicit to avoid `as` casts.
    pub(crate) fn rank(self) -> u8 {
        match self {
            Scope::Global => 0,
            Scope::Session => 1,
            Scope::Agent => 2,
            Scope::Conversation => 3,
            Scope::Turn => 4,
        }
    }
}

/// The canonical context blocks, plus a `Custom` escape hatch for in-band
/// extension. Each variant knows its band, scope, and in-(band,scope) order;
/// [`Context`](crate::Context) is the sole authority on the resulting wire order.
///
/// Derives `Ord` so it can key the context's block map (uniqueness per kind is
/// what makes "duplicate block" unrepresentable). Map order is irrelevant — the
/// render sorts by [`sort_key`](Self::sort_key).
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub enum BlockKind {
    // Band 1 — Static instructions.
    AgentInstruction,
    ToolPolicy,
    // Band 2 — Durable state.
    Soul,
    AssistantIdentity,
    UserProfile,
    GlobalMemory,
    SessionContext,
    SessionMemory,
    AgentMemory,
    SkillList,
    ModeFraming,
    ReasoningEffort,
    ConversationSummary,
    // Band 3 — Volatile tail.
    ToolReminder,
    RecentContext,
    OutputContract,
    /// An extension block placed explicitly within a band/scope. `order`
    /// disambiguates against other blocks sharing the same `(band, scope)`.
    Custom {
        band: Band,
        scope: Scope,
        order: u16,
        label: Cow<'static, str>,
    },
}

impl BlockKind {
    /// The band this block renders in.
    pub fn band(&self) -> Band {
        match self {
            BlockKind::AgentInstruction | BlockKind::ToolPolicy => Band::Static,
            BlockKind::Soul
            | BlockKind::AssistantIdentity
            | BlockKind::UserProfile
            | BlockKind::GlobalMemory
            | BlockKind::SessionContext
            | BlockKind::SessionMemory
            | BlockKind::AgentMemory
            | BlockKind::SkillList
            | BlockKind::ModeFraming
            | BlockKind::ReasoningEffort
            | BlockKind::ConversationSummary => Band::Durable,
            BlockKind::ToolReminder | BlockKind::RecentContext | BlockKind::OutputContract => {
                Band::Volatile
            }
            BlockKind::Custom { band, .. } => *band,
        }
    }

    /// The placement scope used for ordering. (For exception blocks this is the
    /// scope they sort by, not necessarily their architectural ownership scope —
    /// e.g. `OutputContract` sorts in the `Turn` tail by design.)
    pub fn scope(&self) -> Scope {
        match self {
            BlockKind::GlobalMemory
            | BlockKind::Soul
            | BlockKind::AssistantIdentity
            | BlockKind::UserProfile => Scope::Global,
            BlockKind::SessionContext | BlockKind::SessionMemory => Scope::Session,
            BlockKind::AgentInstruction
            | BlockKind::ToolPolicy
            | BlockKind::AgentMemory
            | BlockKind::SkillList
            | BlockKind::ModeFraming
            | BlockKind::ReasoningEffort
            | BlockKind::ToolReminder => Scope::Agent,
            BlockKind::ConversationSummary => Scope::Conversation,
            BlockKind::RecentContext | BlockKind::OutputContract => Scope::Turn,
            BlockKind::Custom { scope, .. } => *scope,
        }
    }

    /// In-(band, scope) order. Disambiguates blocks sharing the same band+scope.
    fn order(&self) -> u16 {
        match self {
            // Band 1
            BlockKind::AgentInstruction => 0,
            BlockKind::ToolPolicy => 1,
            // Band 2
            BlockKind::Soul => 0,
            BlockKind::AssistantIdentity => 1,
            BlockKind::UserProfile => 2,
            BlockKind::GlobalMemory => 3,
            BlockKind::SessionContext => 0,
            BlockKind::SessionMemory => 1,
            BlockKind::AgentMemory => 0,
            BlockKind::SkillList => 1,
            BlockKind::ModeFraming => 2,
            BlockKind::ReasoningEffort => 3,
            BlockKind::ConversationSummary => 0,
            // Band 3
            BlockKind::ToolReminder => 0,
            BlockKind::RecentContext => 0,
            BlockKind::OutputContract => 2,
            BlockKind::Custom { order, .. } => *order,
        }
    }

    /// The total wire-order key: `(band, scope, in-band order)`.
    ///
    /// Public so a caller assembling a *second* channel keyed by the same
    /// taxonomy — e.g. the agent ordering the structured `messages` array it
    /// builds from `ConversationSummary` / `RecentContext` contributions — can sort
    /// by the identical wire order [`Context`](crate::Context) uses for the system
    /// prefix, without duplicating the band/scope/order ranking.
    pub fn sort_key(&self) -> (u8, u8, u16) {
        (self.band().rank(), self.scope().rank(), self.order())
    }
}

/// A context block: a placement (`kind`) plus injected `content`. Empty or
/// whitespace-only content marks the block absent — it renders to zero bytes and
/// [`Context::with`](crate::Context::with) drops the key entirely.
///
/// `content` is a [`Cow`] so callers can pass a borrowed `&str`, an owned
/// `String`, or a `Cow` without ceremony; [`Context`](crate::Context) copies it
/// into owned storage on a real change, so a `Block` never has to outlive the
/// call.
#[derive(Debug, Clone)]
pub struct Block<'a> {
    pub kind: BlockKind,
    pub content: Cow<'a, str>,
}

impl<'a> Block<'a> {
    /// Construct a block from any string-like content (borrowed `&str`, owned
    /// `String`, or a `Cow`).
    pub fn new(kind: BlockKind, content: impl Into<Cow<'a, str>>) -> Self {
        Self {
            kind,
            content: content.into(),
        }
    }
}
