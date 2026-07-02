//! What a tool call *does*, described independently of any tool: a verb, an
//! optional target resource, and a risk class. A tool produces an [`Action`] for
//! each call (its `classify`), and the permission layer evaluates it.

use std::fmt;

/// How dangerous an [`Action`] is, ordered low → high. Policies threshold on it
/// (e.g. "ask at or above [`Moderate`](Self::Moderate)").
///
/// Ordering follows declaration order, so a policy can compare risks directly.
///
/// # Examples
///
/// ```
/// use claw_permission::RiskClass;
///
/// assert!(RiskClass::Safe < RiskClass::Moderate);
/// assert!(RiskClass::High >= RiskClass::Moderate);
/// assert_eq!(RiskClass::default(), RiskClass::Safe);
/// ```
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Default)]
pub enum RiskClass {
    /// No side effects — reads, queries, lookups.
    #[default]
    Safe,
    /// A reversible mutation with low blast radius.
    Low,
    /// A mutation worth a second look (writes, external calls).
    Moderate,
    /// Irreversible or wide-blast-radius — deletes, overwrites, destructive ops.
    High,
}

/// The target an [`Action`] touches, when there is a meaningful one. Used both
/// for policy decisions (e.g. gate a host) and as part of the grant signature so
/// an approval is scoped to *that* resource, not the verb in general.
///
/// The [`Display`](fmt::Display) form is `kind:value`, which is also how it
/// appears inside an [`Action::signature`].
///
/// # Examples
///
/// ```
/// use claw_permission::Resource;
///
/// assert_eq!(Resource::Path("/data/x".into()).to_string(), "path:/data/x");
/// assert_eq!(Resource::Host("example.com".into()).to_string(), "host:example.com");
/// ```
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Resource {
    /// A filesystem path.
    Path(String),
    /// A network host (or URL authority).
    Host(String),
    /// Another agent (by wire id), e.g. a delete/respond target.
    Agent(String),
}

impl fmt::Display for Resource {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Resource::Path(path) => write!(formatter, "path:{path}"),
            Resource::Host(host) => write!(formatter, "host:{host}"),
            Resource::Agent(agent) => write!(formatter, "agent:{agent}"),
        }
    }
}

/// What one tool call does: a `verb` (the tool's own stable label), an optional
/// `resource` it targets, and its [`RiskClass`]. This is the unit the permission
/// layer reasons about — it never sees the tool itself.
///
/// Build with [`new`](Self::new) and refine with [`with_resource`](Self::with_resource):
///
/// ```
/// use claw_permission::{Action, Resource, RiskClass};
///
/// let action = Action::new("write_file", RiskClass::Moderate)
///     .with_resource(Resource::Path("/data/notes.txt".into()));
/// assert_eq!(action.signature(), "write_file:path:/data/notes.txt");
/// ```
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Action {
    verb: String,
    resource: Option<Resource>,
    risk: RiskClass,
}

impl Action {
    /// An action with `verb` and `risk` and no specific resource.
    pub fn new(verb: impl Into<String>, risk: RiskClass) -> Self {
        Self {
            verb: verb.into(),
            resource: None,
            risk,
        }
    }

    /// Attach the resource this action targets (builder style).
    pub fn with_resource(mut self, resource: Resource) -> Self {
        self.resource = Some(resource);
        self
    }

    /// The action's verb (the tool's stable label).
    pub fn verb(&self) -> &str {
        &self.verb
    }

    /// The resource this action targets, if any.
    pub fn resource(&self) -> Option<&Resource> {
        self.resource.as_ref()
    }

    /// The action's risk class.
    pub fn risk(&self) -> RiskClass {
        self.risk
    }

    /// A stable key identifying *this verb on this resource*, used to scope a
    /// granted/denied approval (see `GrantStore`). Two calls with the same verb
    /// and resource share a signature; differing resources do not.
    pub fn signature(&self) -> String {
        match &self.resource {
            Some(resource) => format!("{}:{resource}", self.verb),
            None => self.verb.clone(),
        }
    }
}
