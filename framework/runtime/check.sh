#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

cargo check --workspace --all-targets

if ! cargo public-api --version >/dev/null 2>&1; then
    echo "cargo-public-api is required. Install it with: cargo +stable install cargo-public-api" >&2
    exit 1
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

crates=(
    claw-agent
    claw-api
    claw-cabi
    claw-checkpoint
    claw-context
    claw-core
    claw-interface
    claw-log
    claw-memory
    claw-permission
    claw-sandbox
    claw-skill
    claw-sys
    claw-tool
    claw-utils
)

for crate in "${crates[@]}"; do
    snapshot="snapshots/${crate}.txt"
    current="${tmpdir}/${crate}.txt"
    echo "checking public API snapshot: ${crate}"
    cargo public-api --manifest-path "${crate}/Cargo.toml" --color never -sss >"${current}"
    diff -u "${snapshot}" "${current}"
done
