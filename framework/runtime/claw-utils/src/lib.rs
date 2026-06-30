//! Shared helpers for the claw Rust crates: log-safe text truncation, the
//! prefixed-id newtype macro ([`define_prefixed_id`]), and the process-wide
//! background worker pool ([`SharedTaskPool`]).

use core::fmt;

use thiserror::Error;

pub mod pool;

pub use pool::{PoolConfig, PoolJob, SharedTaskPool};

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
) -> Result<usize, IdParseError> {
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

    rest.parse::<usize>().map_err(|_| IdParseError::Invalid {
        kind,
        value: value.to_string(),
    })
}

/// Define a strongly typed wire-prefixed id (`session-1`, `task-2`, ...).
#[macro_export]
macro_rules! define_prefixed_id {
    ($name:ident, $prefix:literal, $kind:literal) => {
        #[doc = concat!(
            "A `usize` newtype id whose wire form is prefixed with `",
            $prefix,
            "` (e.g. `", $prefix, "1`). Compares, hashes, displays, and (de)serializes by that wire form."
        )]
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
        pub struct $name(
            /// The raw numeric id; the wire form prepends the type prefix.
            pub usize,
        );

        impl $name {
            /// Construct from a raw numeric id.
            pub const fn new(id: usize) -> Self {
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

        impl From<usize> for $name {
            fn from(value: usize) -> Self {
                Self(value)
            }
        }

        impl ::serde::Serialize for $name {
            fn serialize<S: ::serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
                serializer.serialize_str(&self.to_wire())
            }
        }

        impl<'de> ::serde::Deserialize<'de> for $name {
            fn deserialize<D: ::serde::Deserializer<'de>>(
                deserializer: D,
            ) -> Result<Self, D::Error> {
                let value = String::deserialize(deserializer)?;
                Self::from_wire(&value).map_err(::serde::de::Error::custom)
            }
        }
    };
}

#[cfg(test)]
mod tests {
    use super::*;

    const TEST_LIMIT: usize = 96;

    #[test]
    fn display_short_text_unchanged() {
        assert_eq!(
            TruncatedText::with_limit("hi", TEST_LIMIT).to_string(),
            "hi"
        );
    }

    #[test]
    fn with_limit_truncates_with_suffix() {
        let long = "x".repeat(TEST_LIMIT + 10);
        let rendered = TruncatedText::with_limit(&long, TEST_LIMIT).to_string();
        assert_eq!(rendered.len(), TEST_LIMIT + 3);
        assert!(rendered.ends_with("..."));
    }

    #[test]
    fn with_limit_respects_char_boundary() {
        // 50 × 'é' = 100 bytes; a 95-byte limit lands mid-char, so the slice must
        // back off to a boundary rather than panic.
        let text = "é".repeat(50);
        let rendered = TruncatedText::with_limit(&text, 95).to_string();
        assert!(rendered.ends_with("..."));
        assert!(rendered.is_char_boundary(rendered.len()));
    }

    #[test]
    #[cfg(not(target_os = "espidf"))]
    fn new_is_unbounded_on_host() {
        let long = "x".repeat(10_000);
        assert_eq!(TruncatedText::new(&long).to_string(), long);
    }
}
