#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

run() {
    printf '\n==> %s\n' "$*"
    "$@"
}

workspace_flags=(
    --workspace
    --all-targets
)

feature_packages=(
    claw-agent
    claw-api
    claw-cabi
    claw-checkpoint
    claw-context
    claw-interface
    claw-memory
    claw-permission
    claw-sandbox
    claw-skill
    claw-sys
    claw-tool
    claw-utils
)

log_ceiling_features=(
    log_max_off
    log_max_error
    log_max_warn
    log_max_info
    log_max_debug
    log_max_trace
)

trace_ceiling_features=(
    trace_max_off
    trace_max_error
    trace_max_warn
    trace_max_info
    trace_max_debug
    trace_max_trace
)

reasoning_tier_features=(
    reasoning_short
    reasoning_medium
    reasoning_long
)

run_package_all_features() {
    local cargo_cmd="$1"
    local package

    for package in "${feature_packages[@]}"; do
        if [[ "$cargo_cmd" == "clippy" ]]; then
            run cargo "$cargo_cmd" -p "$package" --all-targets --all-features -- -D warnings
        else
            run cargo "$cargo_cmd" -p "$package" --all-targets --all-features
        fi
    done
}

run_claw_log_feature_matrix() {
    local cargo_cmd="$1"
    local feature

    # claw-log's release ceiling knobs forward to log/tracing release_max_level_*
    # features, which are mutually exclusive. Exercise every knob individually
    # instead of using Cargo's workspace-wide --all-features unification.
    for feature in "${log_ceiling_features[@]}"; do
        if [[ "$cargo_cmd" == "clippy" ]]; then
            run cargo "$cargo_cmd" -p claw-log --all-targets --no-default-features --features "$feature" -- -D warnings
        else
            run cargo "$cargo_cmd" -p claw-log --all-targets --no-default-features --features "$feature"
        fi
    done

    for feature in "${trace_ceiling_features[@]}"; do
        if [[ "$cargo_cmd" == "clippy" ]]; then
            run cargo "$cargo_cmd" -p claw-log --all-targets --features "$feature" -- -D warnings
        else
            run cargo "$cargo_cmd" -p claw-log --all-targets --features "$feature"
        fi
    done
}

run_claw_core_feature_matrix() {
    local cargo_cmd="$1"
    local feature

    # claw-core's reasoning tier features are mutually exclusive. Exercise each
    # tier individually with the remaining feature enabled instead of asking
    # Cargo to unify impossible combinations via --all-features.
    for feature in "${reasoning_tier_features[@]}"; do
        if [[ "$cargo_cmd" == "clippy" ]]; then
            run cargo "$cargo_cmd" -p claw-core --all-targets --no-default-features --features "$feature stage_verbose" -- -D warnings
        else
            run cargo "$cargo_cmd" -p claw-core --all-targets --no-default-features --features "$feature stage_verbose"
        fi
    done
}

run cargo fmt --all --check
run cargo clippy "${workspace_flags[@]}" -- -D warnings
run_package_all_features clippy
run_claw_log_feature_matrix clippy
run_claw_core_feature_matrix clippy
run cargo check "${workspace_flags[@]}"
run_package_all_features check
run_claw_log_feature_matrix check
run_claw_core_feature_matrix check
run cargo test "${workspace_flags[@]}"
run_package_all_features test
run_claw_log_feature_matrix test
run_claw_core_feature_matrix test
