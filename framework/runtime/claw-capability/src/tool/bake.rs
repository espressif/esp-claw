use std::fs;
use std::path::Path;

use anyhow::{anyhow, bail, Context, Result};
use serde_json::Value;

const TOOL_DIR_ENTRIES: &[&str] = &["schema.json", "usage.md"];

pub fn validate_tools_dir(tools_dir: &Path) -> Result<usize> {
    println!("cargo:rerun-if-changed={}", tools_dir.display());

    let mut found = 0usize;
    for entry in
        fs::read_dir(tools_dir).with_context(|| format!("reading {}", tools_dir.display()))?
    {
        let entry = entry?;
        let path = entry.path();
        let name = path
            .file_name()
            .and_then(|name| name.to_str())
            .ok_or_else(|| anyhow!("non-UTF-8 entry name in {}", tools_dir.display()))?
            .to_owned();

        if name.starts_with('.') {
            continue;
        }
        if !path.is_dir() {
            bail!("{}: unexpected file '{name}'", tools_dir.display());
        }

        for file in TOOL_DIR_ENTRIES {
            println!("cargo:rerun-if-changed={}", path.join(file).display());
        }
        validate_tool(&path, &name)?;
        found = found.saturating_add(1);
    }

    if found == 0 {
        bail!("no tools found under {}", tools_dir.display());
    }
    Ok(found)
}

fn validate_tool(dir: &Path, dir_name: &str) -> Result<()> {
    ensure_exact_entries(dir, TOOL_DIR_ENTRIES)?;

    let schema_path = dir.join("schema.json");
    let schema_text = fs::read_to_string(&schema_path)
        .with_context(|| format!("reading {}", schema_path.display()))?;
    let schema: Value = serde_json::from_str(&schema_text)
        .with_context(|| format!("parsing {}", schema_path.display()))?;

    let function_name = schema
        .get("function")
        .and_then(|function| function.get("name"))
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("{}: missing function.name", schema_path.display()))?;

    if function_name != dir_name {
        bail!(
            "{}: directory is '{dir_name}' but schema declares '{function_name}'",
            dir.display()
        );
    }

    Ok(())
}

fn ensure_exact_entries(dir: &Path, expected: &[&str]) -> Result<()> {
    let mut actual = Vec::new();
    for entry in fs::read_dir(dir).with_context(|| format!("reading {}", dir.display()))? {
        let entry = entry?;
        let name = entry
            .file_name()
            .to_str()
            .ok_or_else(|| anyhow!("non-UTF-8 entry name in {}", dir.display()))?
            .to_owned();
        if !name.starts_with('.') {
            actual.push(name);
        }
    }

    for want in expected {
        if !actual.iter().any(|name| name == want) {
            bail!("{}: missing required entry '{want}'", dir.display());
        }
    }
    for got in &actual {
        if !expected.iter().any(|want| want == got) {
            bail!("{}: unexpected entry '{got}'", dir.display());
        }
    }
    Ok(())
}
