//! Shared helpers for the claw Rust crates: log-safe text truncation, the
//! prefixed-id newtype macro ([`define_prefixed_id`]), and the id-allocator
//! macro.
//!
//! Async primitives (channels, `block_on`) are intentionally NOT provided here:
//! use vetted crates instead — `async-channel` for channels and
//! `futures_lite::future::block_on` / `edge_executor::block_on` for driving a
//! future to completion.

use core::fmt;

use thiserror::Error;

#[doc(hidden)]
pub mod __private {
    pub use serde;
}

/// Default byte ceiling for [`TruncatedText::new`]. On device, keep trace/log
/// lines compact (flash + UART bandwidth); on host, print the full text so the
/// CLI / offline tooling sees everything. `usize::MAX` makes truncation a no-op.
#[cfg(target_os = "espidf")]
const LOG_SNIPPET_LEN: usize = 96;
#[cfg(not(target_os = "espidf"))]
const LOG_SNIPPET_LEN: usize = usize::MAX;

/// Log-safe view of text: at most `limit` bytes on a char boundary, plus `"..."`
/// when truncated. [`new`](Self::new) uses the platform default
/// ([`LOG_SNIPPET_LEN`]); [`with_limit`](Self::with_limit) overrides it.
pub struct TruncatedText<T> {
    text: T,
    limit: usize,
}

impl<T: AsRef<str>> TruncatedText<T> {
    /// Truncate to the platform default ceiling: compact on device, unbounded on host.
    pub fn new(text: T) -> Self {
        Self {
            text,
            limit: LOG_SNIPPET_LEN,
        }
    }

    /// Truncate to an explicit byte ceiling (call-site override / testable).
    pub fn with_limit(text: T, limit: usize) -> Self {
        Self { text, limit }
    }
}

impl<T: AsRef<str>> fmt::Display for TruncatedText<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let text = self.text.as_ref();
        let mut end = text.len().min(self.limit);
        while end > 0 && !text.is_char_boundary(end) {
            end -= 1;
        }
        write!(f, "{}", &text[..end])?;
        if text.len() > self.limit {
            write!(f, "...")?;
        }
        Ok(())
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Error)]
pub enum IdParseError {
    #[error("empty id string")]
    Empty,
    #[error("invalid {kind} id: {value}")]
    Invalid { kind: &'static str, value: String },
}

pub fn parse_prefixed_id(
    value: &str,
    prefix: &str,
    kind: &'static str,
) -> Result<u32, IdParseError> {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        return Err(IdParseError::Empty);
    }

    let rest = trimmed
        .strip_prefix(prefix)
        .ok_or_else(|| IdParseError::Invalid {
            kind,
            value: value.to_string(),
        })?;

    rest.parse::<u32>().map_err(|_| IdParseError::Invalid {
        kind,
        value: value.to_string(),
    })
}

/// Define a strongly typed wire-prefixed id (`session-1`, `task-2`, ...).
#[macro_export]
macro_rules! define_prefixed_id {
    ($name:ident, $prefix:literal, $kind:literal) => {
        #[doc = concat!(
            "A `u32` newtype id whose wire form is prefixed with `",
            $prefix,
            "` (e.g. `", $prefix, "1`). Compares, hashes, displays, and (de)serializes by that wire form."
        )]
        #[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
        pub struct $name(
            /// The raw numeric id; the wire form prepends the type prefix.
            pub u32,
        );

        impl $name {
            /// Construct from a raw numeric id.
            pub const fn new(id: u32) -> Self {
                Self(id)
            }

            /// Render to the prefixed wire string (e.g. the prefix followed by the number).
            pub fn to_wire(&self) -> String {
                format!(concat!($prefix, "{}"), self.0)
            }

            /// Parse from a prefixed wire string, validating the prefix.
            ///
            /// # Errors
            ///
            /// [`IdParseError`](crate::IdParseError) when the string is empty or
            /// does not carry the expected prefix and a numeric suffix.
            pub fn from_wire(value: &str) -> Result<Self, $crate::IdParseError> {
                $crate::parse_prefixed_id(value, $prefix, $kind).map(Self)
            }
        }

        impl ::std::fmt::Display for $name {
            fn fmt(&self, f: &mut ::std::fmt::Formatter<'_>) -> ::std::fmt::Result {
                write!(f, concat!($prefix, "{}"), self.0)
            }
        }

        impl ::std::str::FromStr for $name {
            type Err = $crate::IdParseError;

            fn from_str(value: &str) -> Result<Self, Self::Err> {
                Self::from_wire(value)
            }
        }

        impl From<u32> for $name {
            fn from(value: u32) -> Self {
                Self(value)
            }
        }

        impl $crate::__private::serde::Serialize for $name {
            fn serialize<S: $crate::__private::serde::Serializer>(
                &self,
                serializer: S,
            ) -> Result<S::Ok, S::Error> {
                serializer.serialize_str(&self.to_wire())
            }
        }

        impl<'de> $crate::__private::serde::Deserialize<'de> for $name {
            fn deserialize<D: $crate::__private::serde::Deserializer<'de>>(
                deserializer: D,
            ) -> Result<Self, D::Error> {
                let value =
                    <::std::string::String as $crate::__private::serde::Deserialize>::deserialize(
                        deserializer,
                    )?;
                Self::from_wire(&value).map_err($crate::__private::serde::de::Error::custom)
            }
        }
    };
}

/// Define a **lock-free** counter that hands out monotonically increasing ids of
/// a [`define_prefixed_id!`]-style newtype.
///
/// The counter is stored as the id newtype itself (not a bare integer) so it
/// follows the id's representation. It deliberately carries **no synchronization
/// and is not `Clone`/`Copy`**: an allocator's whole job is to never repeat, and
/// a copied counter would silently fork into two owners that hand out the same
/// ids. `next` takes `&mut self`, so the borrow checker enforces one live
/// mutator.
///
/// **Synchronization is the caller's decision, added at the caller's layer** —
/// the macro does not bake in an `Arc<Mutex<_>>`, so it never dictates a locking
/// policy or forces a second lock onto a caller that already has one:
/// - **Single `&mut self` owner** → hold it as a plain field; the `&mut` is the
///   exclusivity. No lock at all.
/// - **One field of an already-locked state** (e.g. a store that also owns a map
///   behind a `Mutex`) → put the counter *inside that same lock* so allocation
///   and the state mutation are one critical section — don't add a second lock.
/// - **Genuinely shared across independent owners with no common enclosing lock**
///   (cloned into several holders) → wrap it in the caller's own
///   `Arc<Mutex<_>>` at that one shared owner.
///
/// `next` post-increments: the stored value is the *next* id to hand out. [`new`]
/// starts at `first`; `starting_at` resumes past a known id (e.g. the highest id
/// restored from persistence).
///
/// ```
/// use claw_utils::{define_id_allocator, define_prefixed_id};
///
/// define_prefixed_id!(WidgetId, "widget-", "widget");
/// define_id_allocator!(WidgetIdAllocator(WidgetId), WidgetId(1));
///
/// let mut alloc = WidgetIdAllocator::new();
/// assert_eq!(alloc.next(), WidgetId(1));
/// assert_eq!(alloc.next(), WidgetId(2));
/// // To share one counter across owners, wrap it caller-side, e.g.
/// // `Arc<Mutex<WidgetIdAllocator>>`; the macro itself stays lock-free.
/// ```
#[macro_export]
macro_rules! define_id_allocator {
    ($(#[$meta:meta])* $vis:vis $name:ident($id:ty), $first:expr $(,)?) => {
        $(#[$meta])*
        #[derive(Debug)]
        $vis struct $name($id);

        impl $name {
            /// Start a fresh allocator whose first handed-out id is the macro's
            /// configured first id.
            $vis fn new() -> Self {
                Self::starting_at($first)
            }

            /// Start an allocator whose *next* handed-out id is `first` — the
            /// persistence path: persist the next id, then read it off disk and
            /// pass it here to resume without ever reusing a handed-out id.
            $vis fn starting_at(first: $id) -> Self {
                Self(first)
            }

            /// The id that [`next`](Self::next) will hand out, without advancing.
            ///
            /// Used to persist the allocator's position: write this to disk, then
            /// restore with [`starting_at`](Self::starting_at).
            $vis fn peek(&self) -> $id {
                self.0
            }

            /// Allocate the next id, advancing the counter.
            $vis fn next(&mut self) -> $id {
                let id = self.0;
                self.0 = <$id>::new(id.0.saturating_add(1));
                id
            }
        }

        impl ::std::default::Default for $name {
            fn default() -> Self {
                Self::new()
            }
        }

        impl $crate::__private::serde::Serialize for $name {
            fn serialize<S: $crate::__private::serde::Serializer>(
                &self,
                serializer: S,
            ) -> Result<S::Ok, S::Error> {
                $crate::__private::serde::Serialize::serialize(&self.peek(), serializer)
            }
        }

        impl<'de> $crate::__private::serde::Deserialize<'de> for $name {
            fn deserialize<D: $crate::__private::serde::Deserializer<'de>>(
                deserializer: D,
            ) -> Result<Self, D::Error> {
                let next = <$id as $crate::__private::serde::Deserialize>::deserialize(
                    deserializer,
                )?;
                Ok(Self::starting_at(next))
            }
        }
    };
}
