#!/usr/bin/env bash
#
# Format, lint (clippy), and build the claw Rust workspace for the ESP32-S3
# device target.
#
# Pipeline:
#   1. cargo +stable fmt - format every crate in the workspace (in place).
#   2. cargo +esp clippy - lint the device dependency tree (pass --strict to deny warnings).
#   3. cargo +esp build  - build the device staticlib dependency tree.
#
# "The rust target" is the firmware target xtensa-esp32s3-espidf. `std` is not
# shipped precompiled for *-espidf, so it is built from source with
# `-Z build-std`; the explicit `+esp` toolchain accepts the -Z flag. Only the
# `claw-cabi` aggregator and its dependency tree are device code, so
# clippy/build are scoped to that package.
#
# Usage:
#   ./fmt_lint_build.sh            # release profile (matches the firmware image)
#   ./fmt_lint_build.sh --debug    # dev profile
#   CARGO_BUILD_JOBS=4 ./fmt_lint_build.sh

set -euo pipefail

# Run from the workspace root regardless of the caller's cwd.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# Device firmware target and the aggregator package that the IDF image links.
RUST_TARGET="xtensa-esp32s3-espidf"
DEVICE_PACKAGE="claw-cabi"
BUILD_STD="-Z build-std=std,panic_abort"

# Default to the release profile so this matches the firmware image build
# (see CMakeLists.txt).
PROFILE_FLAG="--release"
PROFILE_NAME="release"
if [[ "${1:-}" == "--debug" ]]; then
    PROFILE_FLAG=""
    PROFILE_NAME="dev"
fi

echo "==> [1/3] cargo fmt (workspace)"
cargo +stable fmt --all

echo "==> [2/3] cargo clippy (${DEVICE_PACKAGE}, target ${RUST_TARGET}, ${PROFILE_NAME})"
# No blanket `-D warnings`: lints the crates intentionally set to `deny` in their
# `[lints]` tables still fail the build, while advisory warnings are reported
# without blocking. Pass --strict to escalate every warning to an error in CI.
CLIPPY_EXTRA=()
if [[ " $* " == *" --strict "* ]]; then
    CLIPPY_EXTRA=(-- -D warnings)
fi
cargo +esp clippy -p "${DEVICE_PACKAGE}" \
    --target "${RUST_TARGET}" \
    ${PROFILE_FLAG} \
    ${BUILD_STD} \
    "${CLIPPY_EXTRA[@]}"

echo "==> [3/3] cargo build (${DEVICE_PACKAGE}, target ${RUST_TARGET}, ${PROFILE_NAME})"
cargo +esp build -p "${DEVICE_PACKAGE}" \
    --target "${RUST_TARGET}" \
    ${PROFILE_FLAG} \
    ${BUILD_STD}

echo "==> Done: fmt + clippy + build succeeded for ${RUST_TARGET} (${PROFILE_NAME})."
