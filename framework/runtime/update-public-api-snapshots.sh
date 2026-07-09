#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if ! cargo public-api --version >/dev/null 2>&1; then
    echo "cargo-public-api is required. Install it with: cargo +stable install cargo-public-api" >&2
    exit 1
fi

mkdir -p snapshots

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
    echo "updating public API snapshot: ${crate}"
    cargo public-api --manifest-path "${crate}/Cargo.toml" --color never -sss \
        >"snapshots/${crate}.txt"
done
