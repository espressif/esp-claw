use claw_utils::TruncatedText;

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
