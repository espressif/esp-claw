//! Configuration + retry surface: [`BackendKind`] string round-tripping
//! (`as_str` / `Display` / `FromStr` / [`ParseBackendKindError`]), every
//! [`ClawApiConfig`] field, and every [`RetryPolicy`] constructor, builder, and
//! field.
//!
//! Run with:
//!
//! ```bash
//! cargo run -p claw-api --example config_and_retry --target x86_64-unknown-linux-gnu
//! ```
//!
//! Pure value manipulation — no transport, no client construction.

use std::str::FromStr;

use claw_api::{BackendKind, ClawApiConfig, ParseBackendKindError, RetryPolicy};

fn main() -> anyhow::Result<()> {
    // ---- BackendKind: stable id, Display, and parsing round-trip -----------
    for kind in [
        BackendKind::OpenAiCompatible,
        BackendKind::AnthropicCompatible,
    ] {
        let id = kind.as_str();
        let shown = format!("{kind}"); // Display
        let parsed = BackendKind::from_str(id)?; // FromStr
        let via_parse: BackendKind = id.parse()?; // str::parse
        assert_eq!(parsed, kind);
        assert_eq!(via_parse, kind);
        println!("backend    -> id={id} display={shown}");
    }

    // An unknown id yields a ParseBackendKindError (usable as std::error::Error).
    match BackendKind::from_str("does_not_exist") {
        Ok(kind) => println!("unexpected -> {kind}"),
        Err(error) => {
            let typed: ParseBackendKindError = error; // Copy
            let as_std_error: &dyn std::error::Error = &typed;
            println!("parse err  -> {typed} / {as_std_error}");
        }
    }

    // ---- ClawApiConfig: constructor + every public field -------------------
    let mut config = ClawApiConfig::new(
        BackendKind::OpenAiCompatible,
        "sk-demo",
        "gpt-4o-mini",
        "https://api.example.com/v1",
    );
    // Request-policy fields are plain and overridable after construction.
    config.timeout_ms = 30_000;
    config.max_tokens = 2_048;
    config.image_max_bytes = 256 * 1024;
    println!(
        "config     -> backend={} key={} model={} url={} timeout={}ms max_tokens={} img_max={}B",
        config.backend.as_str(),
        config.api_key,
        config.model,
        config.base_url,
        config.timeout_ms,
        config.max_tokens,
        config.image_max_bytes,
    );

    // ---- RetryPolicy: constructors, builders, computed backoff, fields -----
    let default = RetryPolicy::default();
    let none = RetryPolicy::none();
    let fixed = RetryPolicy::fixed(3, 250);
    let custom = RetryPolicy::new(4)
        .with_interval_ms(200)
        .with_max_backoff_ms(2_000)
        .with_multiplier(3);

    for (name, policy) in [
        ("default", default),
        ("none", none),
        ("fixed", fixed),
        ("custom", custom),
    ] {
        // Read every public field and the computed per-attempt backoff.
        println!(
            "retry {name:<7}-> retries={} initial={}ms cap={}ms mult={} backoff(1..=3)={:?}",
            policy.max_retries,
            policy.initial_backoff_ms,
            policy.max_backoff_ms,
            policy.backoff_multiplier,
            [
                policy.backoff_ms(1),
                policy.backoff_ms(2),
                policy.backoff_ms(3)
            ],
        );
    }

    Ok(())
}
